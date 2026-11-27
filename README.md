<div align="center">

<img src=".github/meshtastic_logo.png" alt="Meshtastic logo" width="80"/>

<h1>Meshtastic Firmware</h1>

<p>Open-source firmware for long-range, low-power LoRa mesh communication.</p>

<p>
  <a href="https://meshtastic.org/">Website</a> ·
  <a href="https://meshtastic.org/docs/">Documentation</a> ·
  <a href="https://github.com/meshtastic/firmware">Upstream project</a>
</p>

</div>

## About

Meshtastic lets radio devices exchange text messages, location, and telemetry over a decentralized LoRa mesh. A phone or computer can connect to a radio over Bluetooth, Wi-Fi, or USB; mesh communication does not require cellular service or internet access.

This repository contains firmware for a range of embedded and Linux-based devices. It is built with PlatformIO and C++17. Hardware variants define the board-specific radio, display, GPS, and power configuration.

### QZSS L1S support

The `dev-qzss-l1s` branch adds optional QZSS L1S Disaster and Crisis Management Report (DCR) reception for compatible u-blox M10 GNSS receivers. On supported builds, the firmware can decode received DCR messages and surface them to the connected Meshtastic client.

## For device users

- [Check supported devices and get started](https://meshtastic.org/docs/getting-started/)
- [Flash firmware](https://meshtastic.org/docs/getting-started/flashing-firmware/)
- [Open the Meshtastic web flasher](https://flasher.meshtastic.org/)
- [Read the configuration guides](https://meshtastic.org/docs/configuration/)

The web flasher provides released firmware. To use changes from this development branch, build and flash a firmware image from this repository.

## Build from source

Install Git and PlatformIO, then follow the [firmware build guide](https://meshtastic.org/docs/development/firmware/build/). Choose a PlatformIO environment for your board and build it with:

```sh
pio run -e <environment>
```

For example, the LilyGo T-LoRa Pager environment is `tlora-pager`. Build environments and board definitions are under `variants/`.

Available targets include ESP32, nRF52/nRF54, RP2040/RP2350, STM32WL, and native Linux/macOS builds. Feature availability depends on the hardware and build target.

## Development

- `src/` contains the firmware, mesh networking, radio, GPS, and feature modules.
- `variants/` contains board-specific definitions and PlatformIO environments.
- `protobufs/` contains the protocol definitions used to generate message bindings.
- `test/` contains native C++ unit-test suites.

On Linux, run the native unit tests with:

```sh
./bin/run-tests.sh
```

For test setup, contribution guidance, and hardware workflows, see [CONTRIBUTING.md](CONTRIBUTING.md) and the [Meshtastic documentation](https://meshtastic.org/docs/).
