# gicar-serial — Ascaso Baby T Serial Protocol

Source: https://github.com/antondlr/gicar-serial

Reverse-engineered serial protocol for controlling an Ascaso Baby T espresso machine
via its Gicar control board and ESP32-based Bluetooth adapter.

## Key Findings

The machine uses a "Gicar 3d5 Maestro Deluxe Full Range '664' 3SSR" control board,
the same component found in professional multi-group machines. The Bluetooth daughter
board functions as a straightforward serial bridge, making the protocol accessible
to developers.

## Features

- Read and modify machine settings:
  - Power state
  - Coffee and steam temperatures
  - Shot doses and pre-infusion timing
  - Autotimer scheduling
  - Temperature display units
- Python CLI tools for reading state and adjusting parameters
- Supports direct Bluetooth connection, USB serial, or saved response files

## Relevance to La Marzocco Mini Project

Different machine (Ascaso Baby T) and different frame format (no Z/N prefix, 3-digit
address, 115200 baud) but same Gicar controller family. Key shared concepts:
- Temperature stored as value × 10
- Same checksum algorithm: sum(ASCII values, excl CS chars) mod 256
- Same register/offset addressing model

See docs/ for protocol details and memory map.
See python-poc/ for reference implementation.
