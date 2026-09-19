# Folo AI Passport Voice Video Search

Voice-driven video search firmware for the **Folo AI Passport ESP32-C3 badge**. This repository contains the board support for this hardware only and is not intended to run on other ESP32 boards.

[中文 README](README.md)

This project is adapted from version 2.4.2 of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) and preserves the upstream MIT license. See [NOTICE](NOTICE) for the source attribution and modification scope.

## Backend service dependency

The voice video search feature depends on the standalone backend project [westwind027/video-semantic-search](https://github.com/westwind027/video-semantic-search). The backend handles video analysis, scene indexing, semantic retrieval, and accessible preview image URLs. This firmware handles voice capture, ASR through the XiaoZhi protocol, search requests, and result rendering on the device display.

The firmware calls the scene search endpoint by default:

```text
GET /v1/search?q=<URL-encoded query>&limit=12&mode=scene
```

Search results must contain preview image URLs reachable from the device. The firmware first accepts `image_url` and `preview_url`, and also supports the backend's scene-level `scene.preview` field. Relative paths are resolved against the configured service address. The backend's `/static/frames/...` routes support `size=small` and `size=tiny` variants, which the firmware uses for full-screen viewing and result thumbnails respectively.

## Supported features

- ESP32-C3, 8 MB flash, no PSRAM
- 240 × 320 ST7789P3 SPI display with PWM backlight
- ES8311 codec, microphone input, and speaker output
- Three-button GPIO0 ADC resistor ladder
- Device-side Wi-Fi scanning, network selection, and password entry
- ASR results retrieved from the XiaoZhi server while retaining the pairing flow without showing the legacy XiaoZhi conversation page
- Video search requests and result image rendering
- Simplified Chinese resources and the local “你好小智” wake word
- WebSocket, MQTT/UDP, and device-side MCP capabilities

## Hardware connections

| Function | GPIO / parameter |
| --- | --- |
| LCD MOSI / SCLK / CS / DC | GPIO9 / GPIO8 / GPIO1 / GPIO20 |
| LCD RST / backlight | Not connected / GPIO21 |
| ES8311 I²C SDA / SCL | GPIO10 / GPIO7 |
| I²S MCLK / BCLK / WS | GPIO6 / GPIO5 / GPIO3 |
| I²S DOUT / DIN | GPIO2 / GPIO4 |
| Three-button ADC | GPIO0 / ADC1_CH0 |
| USB Serial/JTAG | GPIO18 / GPIO19 |

Default button behavior: on the idle home page, Up and Down change the volume by `+5` and `-5`; OK starts or stops recording. Hold OK for about 850 ms to open Settings. On the search results page, Up and Down select a result; holding Up sends a remote-play command to the backend (opens the current video and seeks to the scene timestamp), while holding Down closes remote playback. Remote control, search, and image transfers share one HTTP worker; a busy worker displays “设备忙” instead of creating concurrent memory pressure. OK starts a new search, while holding OK still opens Settings. In Settings, Up and Down select a menu row and OK enters it; in the volume page, Up and Down adjust the volume and OK returns.

The home page requests `?size=tiny&width=80&height=60` result images. Full-screen viewing requests the backend's native `?size=small` image. The home page reuses a 12 KB JPEG receive buffer and a 96 × 64 RGB565 preview buffer after their first allocation. Full-screen viewing does not allocate a full-screen RGB565 buffer; it reuses the JPEG block buffer and writes decoded MCU blocks directly to the LCD. The display path scales to the screen and supports horizontal and vertical flipping; the board `config.h` contains the display options.

The search endpoint requests up to 12 results. If the response exceeds 16 KB or result parsing cannot allocate its workspace, the firmware releases the request resources and retries with limits `12 → 6 → 3 → 1`, preferring a smaller result set over an unhandled `Search task ran out of memory` failure.

On first boot, or when no Wi-Fi credentials are saved, the device directly shows nearby Wi-Fi networks: use Up and Down to select a network, press OK to open the password keyboard, and select `GO` to connect. Hold OK to return to Settings. The keyboard layout and shortcuts follow [leo-radio](https://github.com/leo0183/leo-radio): a six-column character grid, upper/lowercase and number pages, `<` for deletion, and `GO` for submission. A short Up/Down press moves one key; holding Up/Down jumps one complete row.

The Settings “Service address” page uses the same character keyboard. The default is the safe public placeholder `http://video-search.local:8000`; replace it with the actual backend address, select `GO` to save, or hold OK to cancel and return to Settings. Every menu subpage uses a long OK press to return to its parent. The application does not start the legacy hotspot provisioning page or the legacy XiaoZhi configuration page.

## Build

This project is fixed to ESP-IDF 5.5.5. Do not switch to another IDF version. The board has been build- and flash-tested with this version.

```sh
python scripts/build.py folo/ai-passport-c3 \
  --name folo-ai-passport-c3 \
  --language zh-CN \
  --wake-word nihaoxiaozhi
```

After the build, the complete 8 MB merged image is available at `build/merged-binary.bin`.

## Flashing

Replace `PORT` with the actual serial port:

```sh
python -m esptool --chip esp32c3 --port PORT --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build/merged-binary.bin
```

Flashing overwrites the existing firmware and runtime data in flash. After the first boot, select a Wi-Fi network and enter its password on the device screen.

## Directory layout

- `main/boards/folo/ai-passport-c3/`: badge pins, display, audio, and button initialization
- `main/boards/common/`: common XiaoZhi board abstractions and Wi-Fi support
- `main/audio/`: recording, playback, codecs, and wake word handling
- `main/display/`: LCD/LVGL user interface
- `main/protocols/`: WebSocket and MQTT/UDP protocols
- `scripts/build.py`: the only recommended build entry point

## Current limitations

- The battery icon may be temporarily hidden when CW2017 does not respond or has not completed its first state-of-charge calculation; other features are unaffected.
- Compilation, flash contents, basic startup, and provisioning have been validated. Microphone, speaker, button, and long-term stability testing should continue across different hardware batches.

## License

This project is licensed under the [MIT License](LICENSE). Preserve `LICENSE` and `NOTICE` when distributing derivative versions.
