# Vela streaming voice adapter

This directory isolates the experimental streaming path from the stable
HTTP ASR -> LLM -> TTS flow in `NetworkTest.cpp`.

The transport and 60 ms Opus framing follow the architecture of
`78/xiaozhi-esp32` (MIT licensed). The implementation here uses Espressif's
official `esp_websocket_client` and `esp_audio_codec` components and keeps the
existing ESP32-P4 Function EV Board BSP, UI, and ESP-Hosted networking.

The module is intentionally not connected to the microphone button yet. It
must first pass connection and repeated-frame stress tests against a compatible
WebSocket server.
