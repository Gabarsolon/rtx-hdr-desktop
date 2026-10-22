# RTX HDR Desktop

Applies NVIDIA **RTX HDR** to the whole Windows desktop, or only while one app is in front.

RTX HDR is a driver feature that turns SDR games into HDR. The driver applies it only to programs that draw through a DirectX/Vulkan swap chain. Ordinary desktop apps don't, so they never get it. RTX HDR Desktop captures the screen and draws it again in a swap chain of its own, which the driver does convert.

> **Status: experimental.** The capture, redraw, click-through and capture exclusion all work and are tested. The driver loads RTX HDR (`nvAIHDRx.dll`) into the app. Whether the picture actually looks HDR on your screen is for your eyes to judge, because RTX HDR's output can't be measured from a screen capture (see *How it works*).

## Requirements

- Windows 10 2004 or newer (Windows 11 recommended), **Windows HDR on** (Settings > Display > Use HDR)
- An NVIDIA RTX GPU with a driver that has RTX HDR
- [NVIDIA Profile Inspector](https://github.com/Orbmu2k/nvidiaProfileInspector/releases), to switch RTX HDR on for `rtx-hdr-desktop.exe`

## Setup

1. Build it (see below) or download `rtx-hdr-desktop.exe`.
2. Turn RTX HDR on for the app:
   ```powershell
   .\install-profile.ps1 -ProfileInspector C:\path\to\nvidiaProfileInspector.exe
   ```
   This imports `rtx-hdr-desktop.nip`: a driver profile "RTX HDR Desktop" for `rtx-hdr-desktop.exe` with RTX HDR enabled. You can also do this by hand in Profile Inspector, or in the NVIDIA app if it lists the exe.
3. Start `rtx-hdr-desktop.exe`. Start it a second time if RTX HDR doesn't load right after the profile import (the tray tooltip says *RTX HDR loaded* / *not loaded*).

## Using it

It runs in the tray. There is no window: the screen just looks like it does, but in HDR.

| Key | Does |
| --- | --- |
| **Ctrl+Alt+F10** | Only while the active app is in front / back to the whole screen |
| **Ctrl+Alt+F11** | Pause / resume (compare HDR on and off) |
| **Ctrl+Alt+F12** | Exit |

The tray menu also lets you pick the screen and pick an app, and opens **Settings** with a **sharpness** slider (contrast-adaptive sharpening, after AMD FidelityFX CAS, applied to the SDR picture before RTX HDR; 0 = off, saved in `%APPDATA%\RTX HDR Desktop\settings.ini`). Starting the exe a second time closes the running copy, so one shortcut toggles it.

Options: `--screen N`, `--window "part of a window title"` (only while that window is in front), `--sdr-white NITS`, `--no-calibration`, `--hwnd`, `--list` (print the screens).

## How it works

1. **Capture.** DXGI Desktop Duplication (`DuplicateOutput1`) copies the screen. With Windows HDR on, it arrives as FP16 scRGB.
2. **Back to SDR.** At start, the app flashes a 64-step grey ramp for about 0.1 s and measures what Windows makes of each step. That includes the SDR content brightness slider and any calibration curve (for example an MHC2 profile). It then applies the inverse, so RTX HDR gets the same SDR picture the apps drew.
3. **Present.** It draws that in a click-through, topmost, borderless window that covers the screen, through a flip-model `B8G8R8A8` swap chain (DirectComposition). The driver's RTX HDR profile applies to this swap chain.
4. **No feedback loop.** The window is excluded from screen capture (`WDA_EXCLUDEFROMCAPTURE`), so it never captures itself. For the same reason, it doesn't appear in screenshots or recordings.

What the driver needs, as measured:

- RTX HDR only loads when the window covers **an entire monitor** and is **opaque and unclipped**. 2560×1400 on a 2560×1440 screen: no. A premultiplied-alpha swap chain, a DirectComposition clip or a window region: no.
- Being in the foreground doesn't matter.
- So per-window HDR (only one window's rectangle) isn't possible. "One app" mode puts the whole screen in HDR while that app is in front.
- Screen captures (Desktop Duplication) show the overlay's output identical with RTX HDR loaded or not. Either the driver applies RTX HDR after the point where Windows captures, or it doesn't apply to this window. Your eyes decide; Ctrl+Alt+F11 toggles for a side-by-side.

## Tuning RTX HDR

The NVIDIA app's RTX HDR sliders follow the focused program, and this app never has focus. Set them on the **RTX HDR Desktop** profile in NVIDIA Profile Inspector instead (enable *show unknown settings* if they're hidden), click *Apply changes*, and restart the app:

| Setting | ID | Meaning |
| --- | --- | --- |
| TrueHDR Peak Brightness | `0x00DD48FC` | peak, nits |
| TrueHDR Middle Grey | `0x00DD48FD` | middle grey, nits |
| TrueHDR contrast Control | `0x00DD48FE` | 100 = neutral |
| TrueHDR Saturation Control | `0x00DD48FF` | 100 = neutral |

## Limits

- The mouse cursor stays SDR, because Windows draws it on its own layer above everything.
- DRM-protected video (Netflix etc.) captures as black.
- About one frame of extra latency.
- Exclusive-fullscreen games draw over it (they don't need it anyway).
- It redraws on every screen update (it can't tell its own presents apart from real changes), so it runs at your refresh rate while the screen is changing.
- Screens driven by another GPU (for example a laptop's iGPU panel) work, but the frame is copied through the CPU.
- Text and white pages get very bright. Lower RTX HDR's peak brightness / middle grey in Profile Inspector if it's too much.

## Build

Visual Studio 2022 (C++ desktop workload) and the Windows SDK:

```powershell
.\build.ps1        # -> out\rtx-hdr-desktop.exe
```

## Diagnostics

The log is at `%TEMP%\rtx-hdr-desktop.log`.

- `--selftest SECONDS`: run, then log whether RTX HDR loaded, whether capture exclusion works, and which NVIDIA modules are in the process (exit code 0 = OK).
- `--pattern`: draw a grey ramp instead of the screen. Pair it with a second copy running `--measure SECONDS`, which logs the brightness that reaches the screen capture.

## License

MIT
