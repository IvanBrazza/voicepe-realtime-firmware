# Voice PE Realtime — device firmware

ESPHome firmware that turns a **Home Assistant Voice PE** into the thin,
low-latency audio client half of
**[Voice PE Realtime](https://github.com/TristanBrotherton/voicepe-realtime)**.
Wake-word detection (default: **"Hey Leonard"**, switchable to Hey Jarvis /
Okay Nabu from Home Assistant) runs on-device; everything else — speech
understanding, replies, smart-home control — streams over a local WebSocket to
the backend add-on's OpenAI Realtime session.

## 📖 All documentation lives in the main repo

**→ [github.com/TristanBrotherton/voicepe-realtime](https://github.com/TristanBrotherton/voicepe-realtime)**

- [Getting Started](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/getting-started.md) — flashing this firmware + installing the backend, step by step
- [Configuration Reference](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/configuration.md) — including this firmware's substitutions (wake word model, sensitivity cutoffs, device name, `va_url`)
- [Features](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/features.md) — wake words & the retrain flywheel, speaker recognition, timers, and more
- [FAQ](https://github.com/TristanBrotherton/voicepe-realtime/blob/main/docs/faq.md) — including how to revert to the stock firmware

## Building from source

You don't build anything by hand for a normal install — the per-device stub
([`esphome-builder.dhcp.yaml`](esphome-builder.dhcp.yaml) or
[`esphome-builder.static-ip.yaml`](esphome-builder.static-ip.yaml)) pulls
[`home-assistant-voice.realtime.yaml`](home-assistant-voice.realtime.yaml) from
this repo as a remote package, and ESPHome Builder compiles and flashes it (one
click for updates). To hack on the firmware itself, point the stub's `packages:`
block at your fork/branch instead. Wake-word models live in
[`models/`](models/) (previous model kept in `models/previous/` for rollback).

## EchoMuse satellite variant

[`home-assistant-voice.echomuse.yaml`](home-assistant-voice.echomuse.yaml) is a
separate build variant for an [EchoMuse](https://github.com/wilbowes/EchoMuse)
fleet. It keeps the PE's native ESPHome API/OTA, XMOS AEC, hardware controls,
local announcements, Sendspin, mixer, and headphone handling, while replacing
the realtime backend client with EchoMuse `/control` and `/data` WebSockets.
Wake detection and room arbitration run only on the EchoMuse controller.

Start with [`esphome-builder.echomuse.dhcp.yaml`](esphome-builder.echomuse.dhcp.yaml),
set `echomuse_host` to the controller's stable hostname or LAN address, and add
the normal Wi-Fi, API, and OTA secrets before compiling in ESPHome Builder.
The variant limits compilation to one process so the inherited TensorFlow Lite
Micro dependency does not exhaust the Builder host's memory; the first build
is consequently slower than an ordinary ESPHome image.
Deploy the compatible EchoMuse controller first. Approve the new
`voice-pe-<mac>` device over plaintext, use **Secure link** in its status page,
and verify that it reconnects over WSS before enabling mandatory device TLS.
The native PE device and EchoMuse's synthetic Assist satellite intentionally
appear as two distinct Home Assistant devices.

---
*Based on / inspired by xandervanerven's and maxmaxme's Voice PE work and the
official esphome/home-assistant-voice-pe — with thanks.*
