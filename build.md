# ESP-IDF Build Tool Paths (VS Code ESP-IDF Extension)

This document records the ESP-IDF tool paths currently resolved by the VS Code extension for this project after reinstalling with ESP-IDF Install Manager.

## Selected ESP-IDF

- ESP-IDF version: 5.4.4
- Installation root: /Users/kazus/.espressif
- IDF_PATH: /Users/kazus/.espressif/v5.4.4/esp-idf
- IDF_TOOLS_PATH: /Users/kazus/.espressif/tools
- Activation script: /Users/kazus/.espressif/tools/activate_idf_v5.4.4.sh

## Python Environment

- PYTHON: /Users/kazus/.espressif/tools/python/v5.4.4/venv/bin/python
- Virtual env python3: /Users/kazus/.espressif/tools/python/v5.4.4/venv/bin/python3
- IDF_PYTHON_ENV_PATH: /Users/kazus/.espressif/tools/python/v5.4.4/venv

## Toolchain / Build Tools

- Xtensa GDB: /Users/kazus/.espressif/tools/xtensa-esp-elf-gdb/16.3_20250913/xtensa-esp-elf-gdb/bin
- RISC-V GDB: /Users/kazus/.espressif/tools/riscv32-esp-elf-gdb/16.3_20250913/riscv32-esp-elf-gdb/bin
- Xtensa compiler: /Users/kazus/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin
- RISC-V compiler: /Users/kazus/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin
- ULP toolchain: /Users/kazus/.espressif/tools/esp32ulp-elf/2.38_20240113/esp32ulp-elf/bin
- Ninja: /Users/kazus/.espressif/tools/ninja/1.12.1/
- OpenOCD: /Users/kazus/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260304/openocd-esp32/bin
- OpenOCD scripts: /Users/kazus/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260304/openocd-esp32/share/openocd/scripts
- ESP-ROM ELFs: /Users/kazus/.espressif/tools/esp-rom-elfs/20241011/
- ESP Clang: /Users/kazus/.espressif/tools/esp-clang/esp-18.1.2_20240912/esp-clang/bin

## PATH Composition Used By Activation Script

- /Users/kazus/.espressif/tools/esp-clang/esp-18.1.2_20240912/esp-clang/bin
- /Users/kazus/.espressif/tools/esp-rom-elfs/20241011/
- /Users/kazus/.espressif/tools/esp32ulp-elf/2.38_20240113/esp32ulp-elf/bin
- /Users/kazus/.espressif/tools/esp32ulp-elf/2.38_20240113/esp32ulp-elf/esp32ulp-elf/bin
- /Users/kazus/.espressif/tools/ninja/1.12.1/
- /Users/kazus/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260304/openocd-esp32/bin
- /Users/kazus/.espressif/tools/riscv32-esp-elf-gdb/16.3_20250913/riscv32-esp-elf-gdb/bin
- /Users/kazus/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin
- /Users/kazus/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/riscv32-esp-elf/bin
- /Users/kazus/.espressif/tools/xtensa-esp-elf-gdb/16.3_20250913/xtensa-esp-elf-gdb/bin
- /Users/kazus/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin
- /Users/kazus/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/xtensa-esp-elf/bin
- /Users/kazus/.espressif/tools/python/v5.4.4/venv/bin

## Notes

- This file reflects the EIM-managed layout under /Users/kazus/.espressif.
- If VS Code still references old paths like /Users/kazus/esp/v5.4/esp-idf or /Users/kazus/.espressif/python_env/..., rerun the ESP-IDF extension setup and select v5.4.4 from EIM.
