# Orbit Play

A native Windows 11 PS5 Remote Play client, built on the open-source
[chiaki-ng](https://github.com/streetpea/chiaki-ng) protocol library. It uses Direct3D 11,
hardware video decoding, an HDR10 swap chain and SDL3 for controllers.

## What it does

| Feature | Status |
|---|---|
| 1080p at 60 fps | Yes. This is the most the PS5 sends over Remote Play. |
| HDR10 | Yes, HEVC Main10 straight to an HDR10 swap chain. On SDR displays it can tone-map locally. |
| **AI upscaling** (optional) | NVIDIA RTX Video Super Resolution (RTX 20-series+) or Intel Video Super Resolution (Arc / Iris Xe), the same driver AI that Chrome and VLC use. Upscale to 1440p, 1800p, 2160p or your display's size. |
| **Enhanced upscaling** (any GPU) | Compression-artifact reduction, then an edge-directed upscale and noise-gated sharpening, so the picture stays clean instead of grainy. |
| **Frame generation** (optional) | 2x / 3x / 4x from a 60 fps stream gives 120 / 180 / 240 fps on a fast enough display. It uses hierarchical GPU motion estimation with temporal prediction and occlusion fallback. |
| **AI HDR** | NVIDIA RTX Video HDR turns SDR games into HDR on an HDR display. A shader "HDR boost" works on any GPU. |
| **Ambient lighting** | On ultrawide (or any non-16:9) windows, the bars glow with the picture's edge colors, smoothed over time. |
| Aspect modes, picture modes | Fit, Fill (crop) and Stretch. Natural, Vivid and Cinema presets, plus brightness, contrast, saturation and vibrance. |
| Screenshots | **F12** saves a PNG to `Pictures\Orbit Play`, at the upscaled resolution and tone-mapped if the stream is HDR. |
| Frame pacing graph, auto-reconnect | Shown in the stats overlay (F2). After a Wi-Fi hiccup it reconnects up to 3 times. |
| DualSense haptics | Yes, over USB. Over Bluetooth it falls back to rumble because Windows doesn't expose the haptics audio device over Bluetooth. |
| Adaptive triggers, lightbar, player LEDs | Yes (USB and Bluetooth) |
| Gyro, accelerometer, touchpad | Yes |
| Microphone / voice chat | Yes. Toggle it with the DualSense mute button or from the stream menu. |
| Other controllers | Yes. Xbox, DualShock 4 and generic pads are mapped to PS buttons, and PS5 haptics become rumble. |
| Wake from rest mode, put to rest | Yes |
| On-screen keyboard text, console login passcode | Yes |
| Hardware decoding | D3D11VA, frames stay on the GPU. It falls back to software decoding. |

The console still sends 1080p/60, so upscaling and frame generation happen on your PC. AI super
resolution does reconstruct convincingly sharp detail, but it can't recover detail that the stream's
compression discarded. Generated frames look like real 120/240 fps motion, but they add latency
(about 8 ms at 2x and 12 ms at 4x) and can show artifacts on very fast or chaotic motion. For
competitive games, leave frame generation off.

**NVIDIA tip:** if AI upscaling seems to do nothing, open the NVIDIA App (or NVIDIA Control Panel) →
*RTX Video Enhancement*, turn on **Super Resolution** and pick quality **4**. For AI HDR, turn on
**RTX Video HDR** there and enable Windows HDR.

## First-time setup

1. On the PS5, open **Settings > System > Remote Play** and turn on **Enable Remote Play**.
   To wake the console from rest mode, also turn on **Settings > System > Power Saving >
   Features Available in Rest Mode > Enable Turning On PS5 from Network**.
2. Start Orbit Play. Your PS5 appears under **Consoles**. If it doesn't, add it by IP address
   (find the IP on the PS5 under Settings > Network > Connection Status).
3. Click **Pair**:
   - Click **Sign in with PSN in browser**, sign in with the account you use on the PS5, then
     copy the address of the blank page you land on and paste it into the box. Click **Get ID**.
   - On the PS5, open **Settings > System > Remote Play > Link Device** and type the 8-digit code.
4. Click **Play**.

For the best results, connect the PS5 by Ethernet and use a wired or 5 GHz connection on the PC.

## Keyboard & mouse

Keyboard and mouse play is on by default. When the stream starts, the mouse is captured and
drives the right stick (mouse-look), and a gamepad keeps working at the same time. **Esc** or **F1**
frees the mouse and opens the menu. All bindings can be changed in *Controls*: click a binding,
then press a key, mouse button or wheel direction.

| Default | PS5 |
|---|---|
| WASD | Left stick (hold **Left Alt** to walk) |
| Mouse | Right stick (sensitivity, anti-deadzone and smoothing are adjustable) |
| Left / right mouse | R2 / L2 |
| Space, C, R, F | Cross, Circle, Square, Triangle |
| Q, E | L1, R1 |
| Left Shift, V | L3, R3 |
| 1 2 3 4 | D-pad up / left / down / right |
| Enter, Tab, T, Home | Options, Create, touchpad press, PS |

## Controls while streaming

| Action | Input |
|---|---|
| Stream menu (enhancements, mic, rest mode, disconnect) | **Create + PS** together (View + Guide on Xbox pads), or **F1 / Esc** |
| Fullscreen | **F11** or **Alt+Enter** |
| Statistics overlay | **F2** |
| Cycle upscaling / frame generation | **F3** / **F4** |
| Ambient light on/off, split-screen compare | **F5** / **F6** |
| Sharpness - / + | **F7** / **F8** |
| Picture mode, microphone | **F9** / **F10** |
| Screenshot | **F12** |

Every setting can be changed while you play in the tabbed stream menu. Stream-level settings
(resolution, frame rate, codec, bitrate, HDR) show a **Reconnect now** button that re-joins in a
few seconds.
| Microphone on/off | DualSense **mute button** |

## Settings worth knowing

- **Codec:** use H.265 (HEVC). It looks better at the same bitrate and is required for HDR.
- **HDR:** turn on *Use HDR* in Windows Settings > Display. Orbit Play detects it and requests
  an HDR stream. *SDR / UI brightness* sets how bright SDR content and the menus look in HDR.
- **Bitrate:** *Auto* uses the PS5's 1080p preset. On a solid wired LAN you can raise it.
- **V-Sync off:** gives the lowest latency, but the picture may tear. V-Sync is always on
  while frame generation is active.

Settings and paired consoles are stored in `%APPDATA%\OrbitPlay\settings.ini`, and the log is
in `%APPDATA%\OrbitPlay\orbitplay.log`.

## Building

### GitHub Actions (no local setup)

Push this folder to a GitHub repository. The **Build Orbit Play (Windows x64)** workflow
builds it with MSYS2 and uploads `OrbitPlay-win64.zip` as an artifact.

### Locally with MSYS2

Install [MSYS2](https://www.msys2.org), open the **MSYS2 MINGW64** shell and run:

```sh
pacman -S --needed mingw-w64-x86_64-{cc,cmake,ninja,pkgconf,python,python-protobuf,openssl,opus,json-c,miniupnpc,libevent,sdl3,ffmpeg} git
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OrbitPlay
```

chiaki-ng is fetched automatically at a pinned revision. To use your own checkout, put it
at `third_party/chiaki-ng` or pass `-DCHIAKI_DIR=...`.

## Troubleshooting

- **The console isn't found:** Windows Firewall must allow Orbit Play on private networks (it
  asks on first start). VPN or virtual adapters can also swallow broadcasts, so add the console
  by IP.
- **No haptics:** connect the DualSense by USB cable. The stats overlay (F2) shows
  `Haptics DualSense` when it's active.
- **The stream is SDR on an HDR monitor:** check that Windows HDR is on for that monitor and
  that the codec is H.265.
- **Stutter:** look at the decode fps and lost frames in the stats overlay. Lost frames point
  to the network, so try Ethernet or a lower bitrate.

## License

AGPL-3.0 (with the OpenSSL exception), because it links chiaki-ng's library. See `LICENSE`.
If you distribute builds, you must also make the source available.
