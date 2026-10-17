# Ethos-U55 test (same FaceDecoder, second NPU)

Proves a real model runs on the E8's second NPU: the Ethos-U55 attached to the M55_HP core (NPU_HP, 0x400E1000, 256 MACs).

Result on the DevKit-E8 (Sat Oct 17, 2026):
- Vela put all 9 operators on the NPU, none on the CPU (71 KB program).
- With a zero latent, the first 10 outputs were identical on the U55 and the U85: 0.4806 0.5388 0.5934 0.6444 0.6917 0.7245 0.7427 0.7609 0.7718 0.7754
- Time per face, from 100 `forward()` calls at 400 MHz:
  - Ethos-U85: 249 µs average (248 min, 255 max)
  - Ethos-U55: 385 µs average (384 min, 391 max)
- The driver base address read back as 0x400E1000 (U55); on the normal build it reads 0x49042000 (U85).

## Repeat the test
1. Copy `model_pte.c` and `ai_layer.clayer.yml` from here into `ai_layer/`. To regenerate them instead, run `create_ai_layer.py u55test.cbuild-mlops.yml` with a copy of `cmsis-executorch.cbuild-pack.yml` named `u55test.cbuild-pack.yml`.
2. In `board/DevKit-E8/Board-U85.clayer.yml`, change two lines:
   - `ETHOSU85` → `ETHOSU55`
   - `Ethos-U Driver&Generic U85` → `Ethos-U Driver&Generic U55`
3. Run `cbuild setup ... --update-rte`, then build and flash. The serial log prints `bench: FaceDecoder on Ethos-U55: ...`.
4. Restore the U85 setup with `git checkout -- ai_layer board/DevKit-E8/Board-U85.clayer.yml`.

The Ethos-U driver is compiled for one NPU type, so one firmware drives one NPU. Using both at once would need the second M55 core (M55_HE, with its own U55) running its own firmware.
