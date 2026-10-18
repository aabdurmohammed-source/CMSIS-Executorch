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
#include <cstring>
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

// Temp (NPU scratch) first: it then starts at the bottom of SRAM0, and a large
// scratch arena does not straddle the SRAM0/SRAM1 boundary (0x02400000).
alignas(16) uint8_t g_temp_pool[kTempPoolSize] APP_POOL_ATTRIBUTES;
alignas(16) uint8_t g_method_pool[kMethodPoolSize] APP_POOL_ATTRIBUTES;

constexpr size_t kLatent = 16;
constexpr size_t kFacePixels = 32 * 32;

constexpr size_t kMaxInput = 32 * 32;

// The model's input buffer: a 16-value latent (FaceDecoder) or a normalized
// 32x32 grey crop (FaceMirror); the model's metadata says which (g_in_numel).
alignas(16) float g_latent[kMaxInput];
size_t g_in_numel = 0;
EmbeddedModule* g_module = nullptr;
Tensor* g_input = nullptr;

// Runs the model on g_latent and writes its 32x32 output as 8-bit pixels.
int run_to_pixels(uint8_t* pixels) {
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

}  // namespace

// Provided by the board layer when it defines BOARD_FACE_DEMO (face demo) or
// BOARD_BUNNY_DEMO (bunny companion).
#if defined(BOARD_FACE_DEMO) || defined(BOARD_BUNNY_DEMO)
#define BOARD_DEMO 1
#include "RTE_Components.h"
#include CMSIS_device_header
extern "C" void face_demo_loop(void);
extern "C" void bunny_demo_loop(void);
// Decoder benchmark results in microseconds (debugger-readable).
extern "C" volatile uint32_t g_bench_us[3];  // avg, min, max
volatile uint32_t g_bench_us[3];
// First 10 outputs of the zero-latent test run (debugger-readable, to compare NPUs).
extern "C" volatile float g_test_out[10];
volatile float g_test_out[10];
#if defined(ETHOSU85)
#define NPU_NAME "Ethos-U85"
#else
#define NPU_NAME "Ethos-U55"
#endif
#endif

// Which model the AI layer holds, from its input size.
const char* model_name() {
  return g_in_numel == kLatent ? "FaceDecoder" : (g_in_numel == 30 ? "BunnyDecoder" : (g_in_numel == 32 ? "BunnyDecoderV2" : "FaceMirror"));
}

// Number of input values the embedded model takes: 16 (FaceDecoder, latent
// in) or 1024 (FaceMirror, 32x32 crop in). 0 before app_main has loaded it.
extern "C" int face_model_input_size(void) {
  return static_cast<int>(g_in_numel);
}

// FaceDecoder on the NPU: 16 latent values in, 32x32 8-bit pixels out.
// Returns 0 on success.
extern "C" int face_generate(const float* latent, uint8_t* pixels) {
  if (g_module == nullptr || g_input == nullptr || g_in_numel != kLatent) {
    return -1;
  }
  for (size_t i = 0; i < kLatent; ++i) {
    g_latent[i] = latent[i];
  }
  return run_to_pixels(pixels);
}

// FaceMirror on the NPU: a normalized 32x32 grey crop (values 0..1, row
// major) in, a generated 32x32 face as 8-bit pixels out. Returns 0 on success.
extern "C" int face_mirror(const float* crop, uint8_t* pixels) {
  if (g_module == nullptr || g_input == nullptr || g_in_numel != kFacePixels) {
    return -1;
  }
  for (size_t i = 0; i < kFacePixels; ++i) {
    g_latent[i] = crop[i];
  }
  return run_to_pixels(pixels);
}

// Runs the model on in[0..n_in) (n_in must be the model's input size) and
// points *out at its float output (valid until the next run), *n_out values.
// Returns 0 on success.
#ifdef BOARD_BUNNY_DEMO
// Second program: the doodle classifier (board/DevKit-E8/doodle_pte.c, made by
// model/doodle/export_doodle.py), with its own small memory pools.
extern "C" const unsigned char doodle_pte[];
extern "C" const unsigned long doodle_pte_size;
namespace {
constexpr size_t kDoodleIn = 28 * 28;
alignas(16) uint8_t g_doodle_method_pool[64 * 1024] APP_POOL_ATTRIBUTES;
alignas(16) uint8_t g_doodle_temp_pool[64 * 1024] APP_POOL_ATTRIBUTES;  // Vela scratch ~20 KB
alignas(16) float g_doodle_in[kDoodleIn];
EmbeddedModule* g_doodle = nullptr;
Tensor* g_doodle_tensor = nullptr;
}  // namespace

// Classifies a 28x28 drawing (white strokes on black, 0..1, row major) and
// writes up to n class scores (logits). Returns the number written, or < 0.
extern "C" int doodle_classify(const float* in, float* scores, int n) {
  if (g_doodle == nullptr || g_doodle_tensor == nullptr) {
    return -1;
  }
  for (size_t i = 0; i < kDoodleIn; ++i) {
    g_doodle_in[i] = in[i];
  }
  auto outputs = g_doodle->forward(*g_doodle_tensor);
  if (!outputs.ok() || outputs->empty() || !outputs->front().isTensor()) {
    return -2;
  }
  Tensor t = outputs->front().toTensor();
  if (t.scalar_type() != ScalarType::Float) {
    return -3;
  }
  const float* p = t.const_data_ptr<float>();
  int k = static_cast<int>(t.numel()) < n ? static_cast<int>(t.numel()) : n;
  for (int i = 0; i < k; ++i) {
    scores[i] = p[i];
  }
  return k;
}
#endif

extern "C" int nn_forward(const float* in, int n_in, const float** out, int* n_out) {
  if (g_module == nullptr || g_input == nullptr || static_cast<size_t>(n_in) != g_in_numel) {
    return -1;
  }
  for (size_t i = 0; i < g_in_numel; ++i) {
    g_latent[i] = in[i];
  }
  auto outputs = g_module->forward(*g_input);
  if (!outputs.ok() || outputs->empty() || !outputs->front().isTensor()) {
    return -2;
  }
  Tensor t = outputs->front().toTensor();
  if (t.scalar_type() != ScalarType::Float) {
    return -3;
  }
  *out = t.const_data_ptr<float>();
  *n_out = static_cast<int>(t.numel());
  return 0;
}

extern "C" int app_main(void) {
  executorch::runtime::runtime_init();

  printf("ExecuTorch Ethos-U85 example: %lu byte model\n", model_pte_size);

  // APP_MODEL_SRAM_SIZE: run the model from a copy in SRAM. A large model's
  // weights end up in MRAM above 0x80400000, which the NPU could not read
  // ("NPU error(s) during inference"); the CPU can, so it copies them once.
  const unsigned char* pte = model_pte;
#ifdef APP_MODEL_SRAM_SIZE
  static uint8_t g_model_sram[APP_MODEL_SRAM_SIZE] __attribute__((aligned(16))) APP_POOL_ATTRIBUTES;
  if (model_pte_size <= sizeof(g_model_sram)) {
    memcpy(g_model_sram, model_pte, model_pte_size);
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(g_model_sram), static_cast<int32_t>(model_pte_size));
    pte = g_model_sram;
    printf("model copied to SRAM at %p\n", static_cast<void*>(g_model_sram));
  } else {
    printf("model (%lu bytes) does not fit the SRAM copy (%u bytes), running from MRAM\n",
           model_pte_size, static_cast<unsigned>(sizeof(g_model_sram)));
  }
#endif

  EmbeddedModule module(
      pte, model_pte_size,
      std::make_unique<BufferDataLoader>(pte, model_pte_size),
      std::make_unique<MemoryAllocator>(kMethodPoolSize, g_method_pool),
      std::make_unique<MemoryAllocator>(kTempPoolSize, g_temp_pool));

  // The input shape comes from the model (model/model.py): 1x16 for the
  // FaceDecoder, 1x1x32x32 for the FaceMirror. Both return a 32x32 grey face
  // in [0,1]; an all-zero input is the test input. The tensor only wraps the
  // buffer; no tensor extension (and its std::random_device) is needed for that.
  std::array<SizesType, 4> sizes{1, static_cast<SizesType>(kLatent), 1, 1};
  size_t rank = 2;
  {
    auto meta = module.method_meta("forward");
    if (meta.ok()) {
      auto in_meta = meta->input_tensor_meta(0);
      if (in_meta.ok() && in_meta->sizes().size() <= sizes.size()) {
        rank = in_meta->sizes().size();
        for (size_t d = 0; d < rank; ++d) {
          sizes[d] = in_meta->sizes()[d];
        }
      }
    }
  }
  g_in_numel = 1;
  for (size_t d = 0; d < rank; ++d) {
    g_in_numel *= static_cast<size_t>(sizes[d]);
  }
  if (g_in_numel > kMaxInput) {
    printf("model input of %u values does not fit the input buffer\n", static_cast<unsigned>(g_in_numel));
    return 1;
  }
  printf("model input: %u values (%s)\n", static_cast<unsigned>(g_in_numel),
         model_name());
  std::array<DimOrderType, 4> dim_order{0, 1, 2, 3};
  TensorImpl input_impl(ScalarType::Float, rank, sizes.data(), g_latent, dim_order.data());
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
#ifdef BOARD_DEMO
        g_test_out[i] = p[i];
#endif
      }
    }
    printf("\n");
  }

  printf("Test_result: PASS\n");

  // A board layer can provide face_demo_loop (joystick-driven live faces); it
  // never returns. Targets without one (the FVP) skip it.
#ifdef BOARD_DEMO
  // Time the decoder by itself: 100 forward() calls on the Ethos-U85.
  {
    constexpr int kRuns = 100;
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint64_t total = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < kRuns; ++i) {
      g_latent[i % kLatent] = 0.01f * static_cast<float>(i);
      uint32_t c0 = DWT->CYCCNT;
      auto r = module.forward(input);
      uint32_t c = DWT->CYCCNT - c0;
      if (!r.ok()) {
        printf("bench: forward failed at run %d\n", i);
        break;
      }
      total += c;
      lo = c < lo ? c : lo;
      hi = c > hi ? c : hi;
    }
    uint32_t mhz = SystemCoreClock / 1000000u;
    g_bench_us[0] = static_cast<uint32_t>(total / kRuns / mhz);
    g_bench_us[1] = lo / mhz;
    g_bench_us[2] = hi / mhz;
    printf("bench: %s on " NPU_NAME ": avg %lu us, min %lu us, max %lu us per face (%d runs, CPU %lu MHz)\n",
           model_name(),
           (unsigned long)g_bench_us[0], (unsigned long)g_bench_us[1], (unsigned long)g_bench_us[2],
           kRuns, (unsigned long)mhz);
  }
#ifdef BOARD_BUNNY_DEMO
  {
    // The doodle classifier lives as long as the bunny loop (which never returns)
    // Run from SRAM as well: the NPU reads command stream and weights through
    // its SRAM port (NPU_QCONFIG / NPU_REGIONCFG_0 = 0 in the board layer).
    static uint8_t g_doodle_sram[96 * 1024] __attribute__((aligned(16))) APP_POOL_ATTRIBUTES;
    const unsigned char* dpte = doodle_pte;
    if (doodle_pte_size <= sizeof(g_doodle_sram)) {
      memcpy(g_doodle_sram, doodle_pte, doodle_pte_size);
      SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(g_doodle_sram), static_cast<int32_t>(doodle_pte_size));
      dpte = g_doodle_sram;
    }
    static EmbeddedModule doodle(
        dpte, doodle_pte_size,
        std::make_unique<BufferDataLoader>(dpte, doodle_pte_size),
        std::make_unique<MemoryAllocator>(sizeof(g_doodle_method_pool), g_doodle_method_pool),
        std::make_unique<MemoryAllocator>(sizeof(g_doodle_temp_pool), g_doodle_temp_pool));
    static std::array<SizesType, 4> dsizes{1, 1, 28, 28};
    static std::array<DimOrderType, 4> dorder{0, 1, 2, 3};
    static TensorImpl dimpl(ScalarType::Float, 4, dsizes.data(), g_doodle_in, dorder.data());
    static Tensor dtensor(&dimpl);
    g_doodle = &doodle;
    g_doodle_tensor = &dtensor;
    float scores[16];
    uint32_t c0 = DWT->CYCCNT;
    int nd = doodle_classify(g_doodle_in, scores, 16);
    uint32_t us = (DWT->CYCCNT - c0) / (SystemCoreClock / 1000000u);
    printf("doodle: %lu byte model, test run -> %d classes, %lu us (first run, includes load)\n",
           doodle_pte_size, nd, (unsigned long)us);
    c0 = DWT->CYCCNT;
    nd = doodle_classify(g_doodle_in, scores, 16);
    us = (DWT->CYCCNT - c0) / (SystemCoreClock / 1000000u);
    printf("doodle: classify %lu us on " NPU_NAME "\n", (unsigned long)us);
  }
  bunny_demo_loop();
#else
  face_demo_loop();
#endif
#endif
  printf("\x04");  // EOT: ends the FVP run (semihosting exit); a board just sees a 0x04
  fflush(stdout);
  return 0;
}
