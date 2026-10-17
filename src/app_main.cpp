// Copyright 2026 Arm Limited and/or its affiliates.
// SPDX-License-Identifier: Apache-2.0
//
// Headless ExecuTorch runner for the Ethos-U85 example. Loads the .pte
// embedded by the AI layer (model_pte.h, see create_ai_layer.py), runs one
// inference on the NPU, and prints the output logits. Built entirely from the
// PyTorch::ExecuTorch pack's runtime + operator components; the board layer
// provides main(), stdout and the Ethos-U driver init and then calls app_main.
//
// EmbeddedModule (arm_embedded_module.hpp) manages program loading, method
// memory and execution, like ExecuTorch's Module class does on POSIX hosts.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <executorch/extension/data_loader/buffer_data_loader.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/exec_aten/exec_aten.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/platform/runtime.h>

#include "arm_embedded_module.hpp"
#include "model_pte.h"

using arm::embedded::EmbeddedModule;
using executorch::aten::DimOrderType;
using executorch::aten::ScalarType;
using executorch::aten::SizesType;
using executorch::aten::Tensor;
using executorch::aten::TensorImpl;
using executorch::extension::BufferDataLoader;
using executorch::runtime::EValue;
using executorch::runtime::MemoryAllocator;

namespace {

// Pool sizes and placement are board-overridable: a board layer that cannot
// fit 8 MB of .bss in its default RAM sets APP_*_POOL_SIZE and, via
// APP_POOL_SECTION, the linker section its linker script routes to a larger
// (NPU-accessible) memory.
#ifndef APP_METHOD_POOL_SIZE
#define APP_METHOD_POOL_SIZE (4 * 1024 * 1024)
#endif
#ifndef APP_TEMP_POOL_SIZE
#define APP_TEMP_POOL_SIZE (4 * 1024 * 1024)  // Ethos-U scratch is drawn from here.
#endif
#ifdef APP_POOL_SECTION
#define APP_POOL_ATTRIBUTES __attribute__((section(APP_POOL_SECTION)))
#else
#define APP_POOL_ATTRIBUTES
#endif

constexpr size_t kMethodPoolSize = APP_METHOD_POOL_SIZE;
constexpr size_t kTempPoolSize = APP_TEMP_POOL_SIZE;

alignas(16) uint8_t g_method_pool[kMethodPoolSize] APP_POOL_ATTRIBUTES;
alignas(16) uint8_t g_temp_pool[kTempPoolSize] APP_POOL_ATTRIBUTES;

constexpr size_t kLatent = 16;
constexpr size_t kFacePixels = 32 * 32;

alignas(16) float g_latent[kLatent];  // the decoder's input buffer
EmbeddedModule* g_module = nullptr;
Tensor* g_input = nullptr;

}  // namespace

// Provided by the board layer when it defines BOARD_FACE_DEMO.
#ifdef BOARD_FACE_DEMO
extern "C" void face_demo_loop(void);
#endif

// Runs the decoder on the NPU: 16 latent values in, 32x32 8-bit pixels out.
// Returns 0 on success.
extern "C" int face_generate(const float* latent, uint8_t* pixels) {
  if (g_module == nullptr || g_input == nullptr) {
    return -1;
  }
  for (size_t i = 0; i < kLatent; ++i) {
    g_latent[i] = latent[i];
  }
  auto outputs = g_module->forward(*g_input);
  if (!outputs.ok() || outputs->empty() || !outputs->front().isTensor()) {
    return -2;
  }
  Tensor t = outputs->front().toTensor();
  if (t.scalar_type() != ScalarType::Float || t.numel() != kFacePixels) {
    return -3;
  }
  const float* p = t.const_data_ptr<float>();
  for (size_t i = 0; i < kFacePixels; ++i) {
    float v = p[i] * 255.0f + 0.5f;
    pixels[i] = static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
  }
  return 0;
}

extern "C" int app_main(void) {
  executorch::runtime::runtime_init();

  printf("ExecuTorch Ethos-U85 example: %lu byte model\n", model_pte_size);

  EmbeddedModule module(
      model_pte, model_pte_size,
      std::make_unique<BufferDataLoader>(model_pte, model_pte_size),
      std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool),
      std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool));

  // FaceDecoder takes one latent vector (1x16, see model/model.py) and returns
  // a 32x32 grey face in [0,1]. Zeros give the "average" face. The tensor only
  // wraps the buffer; no tensor extension (and its std::random_device) is
  // needed for that.
  std::array<SizesType, 2> sizes{1, kLatent};
  std::array<DimOrderType, 2> dim_order{0, 1};
  TensorImpl input_impl(ScalarType::Float, sizes.size(), sizes.data(), g_latent, dim_order.data());
  Tensor input(&input_impl);
  g_module = &module;
  g_input = &input;

  auto outputs = module.forward(input);
  if (!outputs.ok()) {
    printf("forward failed (err=%u)\n", static_cast<unsigned>(outputs.error()));
    return 1;
  }
  if (outputs->empty()) {
    printf("forward returned no outputs\n");
    return 1;
  }

  const EValue& out = outputs->front();
  if (out.isTensor()) {
    Tensor t = out.toTensor();
    printf("Output: %u element(s):", static_cast<unsigned>(t.numel()));
    if (t.scalar_type() == ScalarType::Float) {
      const float* p = t.const_data_ptr<float>();
      for (size_t i = 0; i < t.numel() && i < 10; ++i) {
        printf(" %.4f", p[i]);
      }
    }
    printf("\n");
  }

  printf("Test_result: PASS\n");

  // A board layer can provide face_demo_loop (joystick-driven live faces); it
  // never returns. Targets without one (the FVP) skip it.
#ifdef BOARD_FACE_DEMO
  face_demo_loop();
#endif
  printf("\x04");  // EOT: ends the FVP run (semihosting exit); a board just sees a 0x04
  fflush(stdout);
  return 0;
}
