# Face generator for Ethos-U85: a latent vector in, a 32x32 grey face out.
#
# Replaces the Arm TinyCNN example (kept in model_sample_tinycnn.py). Trained by
# model/train_face_decoder.py on the Olivetti faces; the weights are in
# model/face_decoder.pt. create_ai_layer.py quantizes it to int8 and compiles it
# for the NPU.

from pathlib import Path

import torch

from face_decoder import LATENT, FaceDecoder

INPUT_SHAPE = (1, LATENT)


def get_model(input_shape: tuple[int, ...] = INPUT_SHAPE) -> torch.nn.Module:
    model = FaceDecoder()
    weights = Path(__file__).resolve().parent / "face_decoder.pt"
    model.load_state_dict(torch.load(weights, map_location="cpu"))
    return model.eval()


def get_calibration_inputs(
    input_shape: tuple[int, ...] = INPUT_SHAPE, calibration_samples: int = 2
) -> list[torch.Tensor]:
    """Latents drawn like the ones the board will feed in (unit Gaussian)."""
    gen = torch.Generator().manual_seed(0)
    return [torch.randn(input_shape, generator=gen) for _ in range(calibration_samples)]
