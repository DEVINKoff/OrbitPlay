# Orbit Play

A native Windows 11 PS5 Remote Play client, built on the open-source
[chiaki-ng](https://github.com/streetpea/chiaki-ng) protocol library. It uses Direct3D 11,
hardware video decoding, an HDR10 swap chain and SDL3 for controllers.

## What it does

| Feature | Status |
|---|---|
| 1080p at 60 fps | Yes. This is the most the PS5 sends over Remote Play. |
| HDR10 | Yes, HEVC Main10 straight to an HDR10 swap chain. On SDR displays it can tone-map locally. |
| **4K upscaling** (optional, off by default) | Lanczos-2 upscale plus contrast-adaptive sharpening of the 1080p stream to your display resolution. |
| **120 fps frame generation** (optional, off by default) | GPU block-matching motion estimation and a motion-compensated in-between frame for every real frame. Needs a display at 120 Hz or more. |
| DualSense haptics | Yes, over USB. Over Bluetooth it falls back to rumble because Windows doesn't expose the haptics audio device over Bluetooth. |
| Adaptive triggers, lightbar, player LEDs | Yes (USB and Bluetooth) |
| Gyro, accelerometer, touchpad | Yes |
| Microphone / voice chat | Yes. Toggle it with the DualSense mute button or from the stream menu. |
| Other controllers | Yes. Xbox, DualShock 4 and generic pads are mapped to PS buttons, and PS5 haptics become rumble. |
| Wake from rest mode, put to rest | Yes |
| On-screen keyboard text, console login passcode | Yes |
| Hardware decoding | D3D11VA, frames stay on the GPU. It falls back to software decoding. |

The 4K and 120 fps options are local enhancements. The console still sends 1080p/60.
Upscaling makes the image cleaner on a 1440p/4K screen but can't add detail that isn't in the
stream. Frame generation makes motion look smoother, adds about half a frame (~8 ms) of
latency, and can show artifacts around fast-moving edges, so it suits slower games best.

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

## Controls while streaming

| Action | Input |
|---|---|
| Stream menu (enhancements, mic, rest mode, disconnect) | **Create + PS** together (View + Guide on Xbox pads), or **F1 / Esc** |
| Fullscreen | **F11** or **Alt+Enter** |
| Statistics overlay | **F2** |
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
