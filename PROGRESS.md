# PROGRESS

Last updated: Fri Oct 16, 2026 (pre-flight night)

## Machine checklist

| Item | Status |
|------|--------|
| VS Code 1.138.0 | installed |
| Keil Studio Pack, CMSIS Solution, Environment Manager, CMSIS Debugger | installed |
| Python extension (ms-python.python) | installed |
| Git 2.50.1 | installed |
| Python 3.13 (via `py -3.13`) | installed, **use this one** |
| Python 3.14 (default `python`) | installed, **too new** for ExecuTorch (needs >=3.10,<3.14) |
| CMSIS-Toolbox 2.15.0 (cbuild), cmake, ninja, armclang 6.24.0, arm-none-eabi-gcc | downloaded by Environment Manager in `~/.vcpkg` (not on global PATH, only active inside the VS Code workspace) |
| Corstone-320 FVP (avh-fvp 11.32.23) | installed, working |
| llvm-embedded 22.1.0 | installed |
| Docker | missing, not needed on Windows |
| SEGGER J-Link >= 8.42 | missing (board only) |
| Alif SETOOLS >= 1.110 | missing (board only, needs Alif login) |
| `.venv` (PyTorch 2.14 / ExecuTorch) | created with Python 3.13, imports OK |
| Windows long paths | enabled |
| Free disk on C: | ~38 GB |

## Done
- Environment check.
- FVP installed (Environment Manager > Reactivate Environment fixed the 'cmake not found' error).
- Built SSE-320-U85 and ran on Corstone-320 FVP: **Test_result: PASS** (Fri night).
  - FVP prints a harmless-looking 'load application failed ... 0x12200000..0x128A419B' warning, yet the run passes.
  - Command-line recipe (Git Bash): prepend these to PATH from ~/.vcpkg/artifacts/2139c4c6: toolbox/2.15.0/bin, cmake/4.2.1/bin, ninja/1.13.2, armclang/6.24.0/bin. Then:
    `cbuild cmsis-executorch.csolution.yml --active SSE-320-U85 --packs`
    `FVP_Corstone_SSE-320.exe -f board/Corstone-320/fvp_config.txt --simlimit 60 -a out/cmsis-executorch/SSE-320-U85/Debug/cmsis-executorch.axf`

## In progress
- Brainstorming project ideas (shortlist: sound-to-sight for deaf users, privacy fall guardian, gesture slide control, wake cascade). Not chosen yet.
- Venv done. Export flow verified Sat Oct 17: `.venv/Scripts/python.exe create_ai_layer.py cmsis-executorch.cbuild-mlops.yml` (48s) -> rebuild -> FVP Test_result: PASS. Full workflow (prepare/export/integrate/build/run) works on the FVP.

## Blocked / needs Abdul
- Keil MDK Community license: build worked without a prompt; accept it if VS Code asks.
- Board only: Alif account + SETOOLS (set `alif.setools.root` in VS Code), SEGGER J-Link.

## Next
1. Confirm venv + `python create_ai_layer.py cmsis-executorch.cbuild-mlops.yml` works.
2. Pick a project idea (Abdul is going to use a plugin for ideas).

## Research notes (Fri night)
- Direction Abdul wants: mental health / wellbeing. Board has mics, cameras, IMUs, no heart-rate/EDA sensor.
- Arm learning path for this exact board: https://learn.arm.com/learning-paths/embedded-and-microcontrollers/alif-dual-npu-vision/ (face detection on U55 + image classification on U85 at once, Zephyr + ExecuTorch, needs MT9M114 camera on J16 + MW405 display, SETOOLS, macOS host listed, Windows not mentioned).
- No documented mic or IMU examples found for the E8 yet. Ask organizers.
- Candidate: "calm companion" = mic (overwhelming sound) + IMU (breathing/rocking) + RGB LED breathing light. Frame as wellbeing support, not diagnosis.

## Real board (Sat Oct 17)
- SETOOLS 1.112.00 unzipped to C:\alif\setools\app-release-exec; VS Code user setting `alif.setools.root` set to that path.
- J-Link V9.86a installed; sees on-board probe J-Link-OB-E1-AlifSemi (serial 1219380322). Board console/ISP port: COM4. SW4 on SEUART.
- Board SES firmware is v1.112.0 = matches SETOOLS, no updateSystemPackage needed. Part AE822FA0E5597LS0 (E8, Rev A1).
- Built DevKit-E8 target OK (out/cmsis-executorch/DevKit-E8/Debug/cmsis-executorch.axf).
- NEXT: README section 5, install M55_HP debug stub (writes boot table in MRAM, ask first), SW4 to UART4, then Debug and expect Test_result: PASS in Serial Monitor (115200).
- Sat Oct 17 ~10:50: **REAL BOARD PASS.** Debug stub installed (app-gen-toc + `app-write-mram.exe -p -c COM4`, answered y to A0/A1 revision warning), SW4 on UART4, hex loaded with J-Link (`JLink.exe -device AE822FA0E5597LS0_M55_HP -if swd -speed 4000`, cmd: Connect / LoadFile <hex> / r / g). Serial Monitor COM4 115200 printed Ethos-U85 info + `Test_result: PASS`.
- User PATH now includes C:\Program Files\SEGGER\JLink_V986a (needed for VS Code's JLinkGDBServerCL.exe; restart VS Code to pick up).
- IntelliSense squiggles (board_config.h, stdint.h) and "update recommended" hints for RTE_Device.h/linker files are cosmetic; ignore.
- NEXT: choose project (no camera/display in kit; mics, IMUs, LEDs, joystick, Ethernet + laptop UI). Ask organizers about camera and mic/IMU examples.
- Sat Oct 17: added joystick + RGB LED demo for DevKit-E8 (board/DevKit-E8/led_demo.c, hooked from main.c after app_main; `CMSIS Driver:GPIO` component added to Board-U85.clayer.yml; driver pin id = (port<<3)|pin). Built and flashed with J-Link. Awaiting visual confirmation. NOTE: these are uncommitted edits to tracked files; revert with `git checkout board/DevKit-E8` if unwanted. LED polarity unknown (flip LED_ON_LEVEL if colours look inverted).
- Board has NO built-in screen (MIPI display connector only). On-board PDM mic exists (pins.h: "PDM MIC" on P6_7).

## Face generator project (Sat Oct 17, afternoon)
- Kit now has a screen (ILI9806E MIPI-DSI panel, 480x800) and an MT9M114 camera attached.
- Trained a tiny face VAE on Olivetti faces (model/train_face_decoder.py, ~10 min CPU); decoder = model/face_decoder.py + face_decoder.pt. model/model.py now returns it (old sample kept as model/model_sample_tinycnn.py). csolution mlops: input-shape 1x16, 64 calibration samples. Export: 100% NPU ops, 78 KB program.
- src/app_main.cpp: face_generate(latent,pixels) helper; calls face_demo_loop() when BOARD_FACE_DEMO is defined (DevKit layer). (A weak reference got the demo stripped by armlink, hence the define.)
- board/DevKit-E8: face_demo.c (joystick via Driver_IO GPIO15, morph latent, bilinear-scaled face + 16 latent bars on LCD), display.c/.h (SE run-cfg MIPI PHY power + CDC200 start, frame buffer at 0x02300000 in .bss.ai_pool). Layer components added: SOC GPIO (not CMSIS Driver:GPIO, they clash), CDC, MIPI DSI, DPHY, ILI9806E panel.
- tools/face_viewer.py: older serial-stream viewer (not used now; board no longer streams frames).
- FVP target: probably no longer prints the old reference output (model changed). Not re-tested.
- NEXT: confirm face shows on LCD; then camera (MT9M114 over MIPI CSI2, see pack Boards/Templates for camera demo) feeding the NPU.
- Sat Oct 17 ~12:25: **FACES ON THE LCD WORK.** NPU-generated faces (top 480x480) + 16 latent bars (bottom), ~45 fps, joystick morphs.
  - Root cause of the dark screen: Secure Enclave requests all timed out (0xFD) after many debugger resets -> MIPI clocks never enabled. **Fix: unplug/replug the board (power cycle).** If clocks/SE start failing again, power-cycle first.
  - Also needed: CLK_20M enabled at boot (main.c), VBAT DPHY+UPHY power bits (display.c), panel reads bytes as BGR, bilinear shift bug fixed.
  - display.c still has diagnostic prints (pin steps, DPHY) - harmless, can be removed.
  - Frame buffer can be dumped for checking: J-Link `savebin <path>, 0x02300000, 0x119400` (480x800x3, BGR).
- Sat Oct 17 afternoon: **camera + AI face + mic graph on one screen.**
  - Camera: MT9M114 over MIPI CSI2 via Driver_CPI (board/DevKit-E8/camera.c), RTE_Device.h set to 640x480 RGB565 (IMAGE_CONFIG 3). Needed `CMSIS Driver:I2C` + RTE_I2Cx_*_STUCK_LOW_TIMEOUT defines (layer) because RTE_Device.h is the 2.2.0 version.
  - Mic: PDM channels 4+5 with Alif demo_pdm.c settings (board/DevKit-E8/mic.c), needs CLKEN_HFOSCx2 (76.8 MHz) at boot. Background capture, 256 frames @ 8 kHz per block. Works (blocks counting) but levels are small (RMS ~1-10 in the room); graph shows 0-40 dB. Gain may need raising.
  - Layout: camera 480x360 top, AI face 256x256 middle, scrolling mic dB graph bottom.
- Context docs from Abdul: alif-dk-e8-research.md and edge-ai-ideas-and-context.md (rubric: reliability 20, performance 20, E8 architecture 15, sensor fusion 15; must run standalone on board; Arm pico-faces diffusion example on branch hackathon-pico-faces). Project still not chosen.

## Touch screen + LED flash (Sat Oct 17, evening)
- Touch (GT911) shares the I2C1 bus with the camera. The Alif I2C driver keeps only the first callback (the camera's), so the pack's GT911 driver timed out and touch never worked.
- Fix in face_demo.c: the pack driver still does the GT911 reset; register reads are done by polling `Driver_I2C1.GetStatus().busy` (gt_setup / gt_poll). The GT911 answers at address 0x14.
- Touching anywhere turns both RGB LEDs white for ~6 frames and draws a white square at the finger. Debug globals: g_touch_rc (0 = ok), g_touch_count.
- Mic graph confirmed reacting to claps.

## MILESTONE (Sat Oct 17, evening): every component works on the real board
Confirmed by Abdul on hardware, all running together in one firmware:
- LCD screen (480x800 MIPI DSI)
- Camera live view (MT9M114, 640x480)
- AI face generation on the Ethos-U85 NPU (int8 VAE decoder via ExecuTorch)
- Microphone loudness graph (PDM mics, reacts to claps)
- Touch screen (GT911): tap -> white square under the finger
- RGB LEDs flash white on touch
- Joystick morphs the AI face
Next: pick the project idea and combine components (e.g. camera -> AI, mic -> AI). See HANDOFF.md.

## Motion sensor test (Sat Oct 17, evening) - WORKS
- BMI323 IMU on I3C (board/DevKit-E8/imu.c) comes up at the default I/O voltage (no 1.8V switch needed).
- Board tilted/tapped -> LEDs green (IMU motion > 25). Hand waved at camera -> LEDs blue (frame-difference > 12% of grid). Touch -> white.
- Confirmed by Abdul on hardware.

## Performance + second NPU (Sat Oct 17, afternoon)
- FaceDecoder on Ethos-U85: 249 µs per face (avg of 100 runs, 248-255 µs, CPU at 400 MHz). Printed at boot as `bench: ...`. Debug global g_bench_us[3].
- Second NPU CONFIRMED: the same model was exported for the Ethos-U55 (HP, 256 MACs) and ran 100% on the NPU with outputs identical to the U85. 385 µs per face. Details and steps to repeat: tools/u55_test/README.md.
- One firmware drives one NPU type (the driver is compiled per type). Using the U55 and U85 at the same time would need the M55_HE core.
- Backup: local branch working-checkpoint (no remote push, per Abdul).

## Bunny companion - Phase 1: NPU fit + timing (Sat Oct 17, evening) - DONE
Branch `bunny` (from working-checkpoint 432c0e53). Kit + data in model/bunny/ (the 250 MB .npz is git-ignored).
- model/bunny_decoder.py: BunnyDecoder (30 dials -> Linear 256 -> Linear C0*s*s -> 4x [nearest up x2, conv3x3, ReLU] -> conv3x3 -> 3ch sigmoid). Size and width are settings.
- model/model.py: input-shape 1x30 = bunny (BUNNY_SIZE / BUNNY_WIDTH env vars, weights model/bunny/bunny_decoder_<size>_w<width>.pt, random if missing); 1x16 = face generator. csolution mlops now 1x30 / BunnyDecoder.
- src/app_main.cpp reads the input size from the model (as on the camera-mirror branch).
- Random-weight exports, all 100% NPU (12/12 ops; only the float<->int8 conversion at the edges on the CPU):

| Variant | Weights (MRAM) | Vela SRAM scratch | Board ms/frame (U85, incl. output conversion) |
|---|---|---|---|
| 96x96, channels 128-96-64-32 | 1.58 MB | 865 KB | 7.15 ms |
| 64x64, channels 128-96-64-32 | 0.91 MB | 385 KB | 3.28 ms |
| 64x64, channels 64-48-32-16 | 0.37 MB | 193 KB | 1.42 ms |

- The NPU is not the bottleneck: even 96x96 is fast enough for 30+ fps. Training time on the laptop CPU is (96 full: ~15 min/epoch; 64 full: ~7.5 min/epoch).
- Next: Phase 2 training.
