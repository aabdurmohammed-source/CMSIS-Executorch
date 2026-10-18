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

## Bunny companion - Phase 2 (training) + Phase 4 (firmware loop) (Sat Oct 17, evening)
- Size chosen: 48x48, half width (channels 64-48-32-16), per Abdul. 100% NPU, 246 KB weights, 109 KB scratch, 0.88 ms per frame on the U85 (startup bench).
- model/train_bunny.py trains on a clock (TRAIN_MINUTES, default 17), all 38k training frames, L1 + 0.2 MSE with 5x weight on dark outline pixels, keeps Windows awake. Result: 17.3 min, ~6.1 epochs, val L1 0.0246 (0.0453 after epoch 1). Grids: model/bunny/runs/val_grid.png, blend_grid.png. Body, colours, ears, arms, lying down learned well; eyes/mouth still soft.
- Firmware (BOARD_BUNNY_DEMO replaces BOARD_FACE_DEMO in the board layer; face_demo.c is compiled out, restorable):
  - board/DevKit-E8/bunny_clips.c/.h: C port of all 16 clips.py clips + dial ranges/normalisation.
  - board/DevKit-E8/bunny_demo.c: state machine (idle, wander walk/run, actions, lie down -> sleep after 20 s, wake on any input), 0.45 s smoothstep clip blending + low-pass on dials, NN every frame, bilinear 48 -> 240 px sprite with flip, CPU effects (zzz, sweat, hearts), touch dot, mic graph, LEDs by mood.
  - Inputs: tap bunny = love, tap elsewhere = run there, hold on bunny 1.2 s = hatch (random hue/sat/ear_len/head_scale), IMU shake = shake, loud sound = ears perk, joystick A/B/C/D = wave/yawn/hop/clap, centre = eat.
  - app_main.cpp: nn_forward() generic call; bunny_demo_loop() under BOARD_BUNNY_DEMO.
- Measured with the epoch-1 weights: 50 fps display loop, NN 0.92 ms in the loop.
- Blocked: J-Link "cannot connect to the probe" after the board lost power; USB device present. Needs a real unplug/replug. Final-weights firmware is built, not yet flashed.
- Recovered (Sat 17:20): J-Link still "cannot connect to the probe", but the SE answered over COM4 (M55-HP booted, power normal). Flashed the final 48x48 bunny through the SE with SETOOLS (new app TOC, HP_APP = our firmware at 0x80200000). Board runs again. Until J-Link works: tools/flash_se.sh (SW4 = SE to flash, then SW4 = U4 + replug). Gotcha added to HANDOFF.
- Sat ~18:15: switched the board to the 64x64 bunny (Abdul's call; 18.7 min training, ~4 epochs, val L1 0.028; 1.43 ms/frame on the U85, 361 KB weights, 193 KB scratch, 45 fps). Sprite 448 px, ground at y=690, hit box = middle of the sprite, wander may leave the screen edges.
- Fixes: off-panel touch coordinates ignored (stopped one freeze); hold-to-hatch now works (finger counts as lifted only after 0.15 s without a report; GT911 sends gaps); 1 s delay before display init (cold USB power-on hung on blue; RESET always worked); boot LEDs (red/yellow/blue/green, blinking red = display failed, blinking purple = fault) + HardFault reporter on UART; per-stage frame timing (prints stages of any frame > 150 ms); IMU read every 3rd frame and switched off after 3 slow reads, touch paused 1 s after 3 failed reads (a remaining freeze looked like a sensor read blocking, not a fault).
- Sat ~19:30: J-Link fixed by installing SEGGER's USB driver (JLink_V986a\USBDriver\InstDrivers.exe); the probe had been on Microsoft's generic WinUSB driver and stopped opening after the board lost power. "Failed to halt CPU" right after = unplug 10 s. Flashing over J-Link again (4.6 s).
- Watchdog build (3 s WDT fed every frame, stack high-water in the status line, faster row-blended sprite draw): 36-42 fps with the 448 px 64x64 bunny, stack 1.6 KB of 32 KB (overflow ruled out). 6-minute play test with 36 touches + hatch: no freeze, no watchdog reset.

## Camera world (Sat Oct 17, evening) - built, waiting on hardware
- Bottom-right button (camera icon at home, house icon in the camera world) switches worlds; the mic graph is narrower to make room. g_force_world (debugger-writable) presses it remotely.
- Camera world: live camera fills the screen above the graph (height-fit, centre crop), bunny at 224 px with white keyed out (see-through), composed row by row with the camera so the bunny never flickers. AR anchor: gyro (AR_AXIS 1, AR_SIGN -1, 6 px/deg, deadband 3 deg/s) shifts the bunny against board turns, drifting back at 0.5/s. Touch, hatch, shake, sleep work in both worlds.
- Found: a J-Link disconnect can switch off the DWT cycle counter, which stops every timeout and the animation clock -> the loop now re-enables it every frame.
- Camera frames are ALL ZERO (capture completes without errors). Same symptom as this morning's loose camera cable (J16, underside) -> check the cable.
- Continuous CaptureVideo gave a CPI bus error and broke the NN (1 us = forward failing) -> not used. Snapshot capture started early and collected at draw time instead (snapshot mode measured 14 fps when blocking; video mode 33 fps).
- Last build (async snapshot) is built but NOT flashed: J-Link "Attach to CPU failed" -> needs a USB power cycle.

## Bunny colour bug FIXED (Sat Oct 17, night)
- Symptom: on the board the bunny was flat grey with saturated blue blobs (not a panel issue: the framebuffer had the same pixels).
- Cause: the int8 model output had only 2 distinct values (0.5 and 1.0). Training drove the white background's final-conv logits up to ~1774 while colours live in about -2..+2, so int8 (per-tensor, 256 steps) rounded every colour to sigmoid(0) = grey. Then the remaining speckle came from 8-bit activations in the middle layers.
- Fix 1 (model/bunny_decoder.py): sigmoid(clamp(logits, -6, 6)); for export prepare_for_int8() shifts the last conv bias by +6 and uses hardtanh(0, 12) - 6, the form the Arm quantizer fuses into the conv. No retraining: same maths for the trained weights. model.py calls it.
- Fix 2 (create_ai_layer.py): AI_LAYER_A16W8=1 = 16-bit activations, 8-bit weights. Float-vs-quantized mean abs error 0.037 -> 0.003 (int8 head-fixed) ; visually identical to float.
- Export: `AI_LAYER_A16W8=1 BUNNY_SIZE=64 .venv/Scripts/python.exe create_ai_layer.py cmsis-executorch.cbuild-mlops.yml` -> 13/13 ops on the U85, 364 KB weights, 385 KB scratch, 2.12 ms/frame on the board (was 1.42 ms in int8), 32 fps loop.
- Applies to any new bunny size (e.g. the 128x128 run): same export command, no training change.

## Doodle recognition (Sat Oct 17, night) - WORKS (tested remotely)
- Data: Google Quick, Draw! (CC BY 4.0) 28x28 bitmaps, first 20k per class via HTTP range requests (model/doodle/get_data.py, data git-ignored). Classes: star, sun, house, cat, flower, fish, smiley face, lightning (no "heart" class exists).
- Model: model/doodle/doodle_net.py (3x [conv3x3, ReLU, maxpool] + 2 linear), trained 6 epochs with shift/scale/rotate/thickness augmentation: 96.4% val accuracy (8000 held-out). Export: model/doodle/export_doodle.py -> board/DevKit-E8/doodle_pte.c, int8, 8/8 ops on the U85, 77 KB weights, 20 KB scratch, int8 agrees with float on 99.7%.
- Firmware: second ExecuTorch program in app_main.cpp (doodle_classify(), own 256 KB pools). 0.28 ms per drawing on the U85 (0.36 ms first run). Two models share the U85 now: BunnyDecoder 2.1 ms + DoodleNet 0.3 ms.
- Bunny UI: drag on empty screen = draw (tap still = run there); 0.9 s after lifting the finger the strokes are rasterised like Quick, Draw! (fit to 24 px, centred, ~1.5 px lines, 4x supersampled) and classified; bubble "STAR!" etc. (5x7 font), bunny faces the drawing and reacts (star clap, sun stretch, house hop, cat/lightning shake, flower love, fish eat, smiley wave; < 45% = "???" + sad). Drawing clears after 3 s. Ink layer (1-bit mask) keeps the drawing visible under/around the bunny without flicker, also over the camera.
- Remote test hook g_test_doodle (1 star, 2 house, 3 smiley): STAR 98%, HOUSE 97%, SMILEY FACE 98%.
- Note: J-Link savebin screenshots take seconds; rows read late can show a later frame.

## Cloud-trained bunny v2 (Sat Oct 17, night) - 96x96 ON THE BOARD
- New kit (model/bunny/kit_v2): 32 dials (+ brow, dizzy), 19 clips (+ angry, dizzy, confused), cloud train_bunny.py Decoder (fc1/fc2 + 4 [nearest x2, conv3x3, ReLU], channels 128-128-64-64-32). Weights: model/bunny/decoder_v2_96.pt and decoder_v2_128.pt ("decoder_final.pt"/"(2)" = 96, "(1)" = 128 - they were NOT duplicates). dial_ranges_v2.json = NN normalisation.
- model/bunny_decoder_v2.py loads them as-is + the int8-safe head; model.py: input-shape 1x32 -> DecoderV2 (BUNNY_SIZE 96/128). Calibration from the kit's clips (cairosvg stubbed).
- Export: `AI_LAYER_A16W8=1 BUNNY_SIZE=96 .venv/Scripts/python.exe create_ai_layer.py cmsis-executorch.cbuild-mlops.yml` -> 13/13 ops on the U85, 1.46 MB weights, 1.73 MB scratch, **10.6 ms per frame**, loop 18 fps. Picture quality is much better (clean eyes, mouth, blush).
- Firmware: 32 dials, normalisation from dial_ranges_v2.json, clips angry/dizzy/confused; shake -> dizzy, unsure doodle -> confused, lightning doodle -> angry; MAX_NN 128, sprite stays 448 px.
- 128x128: 100% NPU, 2.35 MB weights, 3.0 MB scratch, but the app no longer fits the 2 MB MRAM code region; after enlarging it the NPU failed ("NPU error(s) during inference", forward err 35) - the model sat above 0x80400000 (the former MRAM user region; likely not readable by the NPU / firewalled). Reverted to the 2 MB region; 96x96 is the shipped model. 128 needs weights in a NPU-readable place (e.g. OSPI or SRAM) - later.

## 128x128 bunny WORKS (Sat Oct 17, late night)
- Root cause of "NPU error(s) during inference" for big models: NPU status 0x804 = bus abort on the EXT AXI port at the first command (qread 0). The Ethos-U driver defaults fetch the command stream (NPU_QCONFIG 2) and weights (NPU_REGIONCFG_0 3) through the EXT port, which on this board only reaches low MRAM (the 96x96 model fit there; the 2.4 MB 128x128 model reached past 0x80400000). Firewall config was open (ruled out); SRAM bank boundary ruled out.
- Fix: (1) firmware copies the model into SRAM at boot (APP_MODEL_SRAM_SIZE 0x280000, app_main.cpp) - also the doodle model; (2) driver defines NPU_QCONFIG=0, NPU_REGIONCFG_0=0 (SRAM port) in the board layer; (3) Vela system Ethos_U85_SRAM_Only / memory Sram_Only (csolution mlops). App MRAM region enlarged to 0x80200000-0x80560000 (TOC/stubs from 0x8057C890).
- Result: DecoderV2 128x128, a16w8, 13/13 ops on the U85, 2.58 MB program, 3.0 MB scratch: **16.7 ms/frame**, loop 16 fps. Doodle 0.23 ms (from SRAM). Bulk SRAM ~7.97 of 8 MiB used (model copy 2.5, scratch 3.06, method 0.5, LCD 1.1, camera 0.59, doodle 0.22).
- Export: `AI_LAYER_A16W8=1 BUNNY_SIZE=128 .venv/Scripts/python.exe create_ai_layer.py cmsis-executorch.cbuild-mlops.yml`

## Back to 96x96 + bold lines (Sat Oct 17, late night)
- Shipped model: DecoderV2 96x96 (Abdul's choice), re-exported with the SRAM-only Vela config and run from an SRAM copy: **9.4 ms/frame** (was 10.6 ms from MRAM), loop 18 fps. APP_MODEL_SRAM_SIZE 0x1A0000, APP_TEMP_POOL_SIZE 0x1C0000 (128x128 values in the clayer comments; 128x128 still works by re-exporting with BUNNY_SIZE=128 and restoring those sizes).
- Bold lines (bunny_demo.c convert_output): after the NN, pixels with luminance < 150 are pushed toward a deep purple (LINE_DARKEN 0.75) and dark pixels darken their 4 neighbours (LINE_THICKEN 0.65) -> outline/eyes/mouth darker and ~1 px thicker at 96 px (~4-5 px on screen). Tunable #defines.

## The bunny's world: drawings come to life (Sat Oct 17, late night)
- A recognised drawing becomes a world object made of its own strokes (bunny_demo.c, "the bunny's world"): it shrinks and moves into place over 0.8 s, kept in the ink mask (no flicker). Up to 4 objects (oldest replaced; one sun, one house).
  - fish -> drops to the floor; bunny walks over and eats it bite by bite (2.8 s), "YUM!", food +50%
  - flower -> planted on the floor; bunny walks over and sniffs it (love + hearts)
  - house -> stands on the floor; when sleepy (20 s idle) the bunny walks home and lies down next to it
  - sun -> floats to the top-left sky; sets (slides down, vanishes) at bedtime
  - star -> floats into the sky (clap)
  - cat -> on the floor for 8 s; bunny startled (shake) then runs to the far side
  - lightning -> stays 1.8 s, LEDs flicker white, bunny angry
  - smiley -> floats up, fades after 3.5 s, bunny waves
  - unsure -> "???", confused, drawing fades
- Feeding mini-game: food meter top-left drains over 3 min (not while asleep); below 30% the bunny says "HUNGRY!" and looks sad every 25 s; fish refills.
- Eraser button bottom-left clears all drawings/objects (mic graph now sits between the two buttons).
- Tested remotely: house 97% (hop, on floor), star 98% (clap, in sky), fish 99% -> "reached the fish" -> eat -> "ate the fish, food 100%". Loop 18-20 fps, NN 9.5 ms. Remote test hook g_test_doodle: 1 star, 2 house, 3 smiley, 4 fish.

## Glitch fix: double buffering (Sat Oct 17, late night)
- Bugs (Abdul): bunny whiting out / half-drawn mid-move, drawings with white boxes around them glitching against the bunny, flickering zzz. Cause: everything was drawn straight into the shown frame buffer (white-clear then redraw, sprite with a white square, white boxes to erase moving objects).
- Fix: display.c double buffering (display_enable_double_buffer): a second 1.1 MB frame buffer; display_flush() cleans the cache, requests CDC200_FRAMEBUF_UPDATE_VSYNC and waits for the CDC to clear the layer reload bit (switch done at vertical blank) before swapping. The face demo stays single-buffered.
- bunny_demo.c now redraws each frame whole into the hidden buffer: fast white clear (memset) or camera background -> drawings and world objects as plain lines (world_draw) -> bunny with white see-through (keyed sprite) -> effects (zzz, hearts) -> bubble, food meter, graph, buttons -> swap. The ink mask / white-box clearing is off in this mode.
- Result: no tearing or white flashes, drawings have no background, clean zzz; 20 fps (was 18). SRAM: + 1.1 MB (both frame buffers fit with the 96x96 model).

## Shoulder ride: BlazeFace on the NPU (Sat Oct 17, late night) - WORKS ON THE BOARD
- Model: Google's BlazeFace front model (MediaPipe), PyTorch port by M. Hollemans (github.com/hollance/BlazeFace-PyTorch, MIT) in model/blazeface/download. model/blazeface/export_blazeface.py: wrapper outputs 1x896x17 (16 box/keypoint regressors + score per anchor), a16w8 (calibrated on Olivetti faces pasted on random backgrounds; score diff vs float 0.0005), 112/112 ops on the U85, 251 KB weights, 1 MB scratch -> board/DevKit-E8/face_pte.c + face_anchors.h.
- Firmware: third ExecuTorch program in app_main.cpp (face_input(), face_detect()), run from an SRAM copy; it SHARES the bunny's NPU scratch pool (models never run at the same time). 10.3 ms per detection.
- Camera world: every 2nd camera frame the CPU crops the centre 480x480 of the 640x480 frame to 128x128 (RGB -1..1), BlazeFace runs, the CPU decodes the best face (weighted blend of overlapping anchors) and maps it to screen coordinates. Shoulder = face centre +-1.1 face widths (bunny's side), +1.25 face heights, smoothed. Face seen 1 s -> bunny jumps in an arc (0.8 s) onto the shoulder and rides it (sit); face lost 2 s -> hops down. Thin green face box (FACE_BOX debug). Camera-world limits: bunny NN on alternate frames, no drawing (taps still run), detector every 2nd frame.
- Camera is fine (earlier black frames: nothing in view / dark). Tested on Abdul: face 99%, "jumping onto the shoulder", riding; screenshot shows the bunny on the shoulder. Camera world 11 fps (home 20 fps).
- Frames are never stored; detection only, no identification.
- Shoulder ride v2: BlazeFace keypoints decoded (eyes/nose/mouth/ears) -> head roll; face smoothed (EMA 0.5) with glitch rejection (jump > 1.5 face sizes ignored unless it repeats); shoulder side locked at the jump (switches only if that shoulder leaves the screen); offsets 0.95 face widths out, 1.05 face heights down (1.35/1.15 floated past the shoulder). Gaze: in the camera world he looks at your face (look_x/look_y, head tilt); riding he copies your head tilt; very close = wide eyes. Camera world 12 fps.

## Personality engine (Sat Oct 17, late night)
- White halo fix: alpha matte (flood fill of near-white from the image border = background; edge pixels get partial alpha and the white un-mixed from their colour; background colour set dark so bilinear never mixes in white); sprite alpha-blended over camera / drawings / white.
- Needs: energy (drains awake, faster when running/hopping; refills asleep), fun (drains when ignored; touches, doodles, faces in view raise it), affection (drains; petting +0.35, faces in view raise it), food (meter). Mood (valence) shows on the face during idle/look/sit/walk: happy eyes, sad eyes + droopy ears, sleepy eyes.
- Choices: every 2.5-6 s the bunny scores 9 actions (walk, look, sit, hop, clap, wave = attention seeking, stretch, yawn, pout) from his needs x learned preference and samples one (softmax, temperature 0.22): not predictable, but fits his mood.
- Learning: petting within 4 s after a spontaneous action multiplies its preference by 1.35 (max 3), bubble "MORE HOPS!" etc.; shaking within 4 s multiplies by 0.75. Status line prints the preferences.
- Sleep: when energy < 12% (or idle 45 s with energy < 60%), walks home if there is a house; wakes by himself when rested (energy > 97%), or on any input.
- Thoughts: when a need is low, a bubble "SLEEPY" / "PET ME?" / "BORED!" every 25-40 s.
- Deformed-bunny fix: the mood blend could push eye weights negative when sad + sleepy (+ happy) added up to > 1 -> out-of-training dial vectors -> warped faces. Now mood strength is capped at 60% and, right before every NN call, eye/mouth weights are clamped >= 0 and renormalised to sum 1 and all scalars clamped to their trained range. Bold lines skip reddish pixels (open mouth / tongue stayed a black blob) and thicken less (0.4); EDGE_FRAC 0.15 keeps his body on screen.
- Yawn "flattening" + a crash FIXED: the C yawn clip did powf(sinf(PI*1.0), 1.5); in float sinf(PI) is -8.7e-8, so powf returned NaN at the end of every yawn cycle -> NaN dials -> the NN drew a squashed, smeared bunny (seen in screenshots), and a NaN reaching the runtime's quantize step can stop it (HardFault with HFSR DEBUGEVT = a failed runtime check, found with the CPU parked in bunny_fault_report). Fix: clamp the sine to >= 0; and before every NN call any non-finite dial is replaced (sanitize step). Verified: 8 frames over a full yawn are clean.
- Touch dying (every read failing, "3 failed reads, pausing 1 s" forever): after two failed pauses in a row the firmware now aborts the I2C1 transfer, does an I2C bus clear (clocks a stuck SDA free), powers the GT911 off and runs its reset/detect again (gt_recover). Not yet seen recovering live.
- Test hooks: g_force_clip (play a clip by number), g_test_doodle, g_force_world; NOTE J-Link Commander numbers are hex (w4 addr, 10 = 16 = angry; use 0x0A for yawn). set_clip() logs "clip: <name>".
