# Seek Thermal Compact Pro (Android) Bootloader

**A reverse-engineered and reconstructed first-stage bootloader for the Seek Thermal Compact Pro (Android variant, product code UQ-AAA), built on NXP LPC43xx (ARM Cortex-M4) and running from external SPIFI flash.**

This repository is a from-scratch C reconstruction of the Compact Pro (Android) LPC43xx boot image, recovered by disassembling a 4 MB SPI-NOR flash dump. The reconstruction is faithful to the original machine code (the disassembly is the source of truth). The dump contains no function symbols; the labels here were assigned manually during analysis from behavior and call context, then kept consistent so the source lines up 1:1 with the listing.

---

## Table of contents

1. [What this is](#1-what-this-is)
2. [Security notice (read first)](#2-security-notice-read-first)
3. [At a glance](#3-at-a-glance)
4. [Target hardware](#4-target-hardware)
5. [Repository layout](#5-repository-layout)
6. [What is reconstructed vs. imported](#6-what-is-reconstructed-vs-imported)
7. [Manually assigned function labels](#7-manually-assigned-function-labels)
8. [Build prerequisites](#8-build-prerequisites)
9. [Building in MCUXpresso](#9-building-in-mcuxpresso)
10. [Memory map](#10-memory-map)
11. [The build identity](#11-the-build-identity)
12. [Boot flow, end to end](#12-boot-flow-end-to-end)
13. [Image format](#13-image-format)
14. [The slot system](#14-the-slot-system)
15. [Cryptographic scheme](#15-cryptographic-scheme)
16. [Keys and where they live](#16-keys-and-where-they-live)
17. [Module reference](#17-module-reference)
18. [Configuration constants](#18-configuration-constants)
19. [Naming conventions and known misnomers](#19-naming-conventions-and-known-misnomers)
20. [Reconstruction caveats (load-bearing assumptions)](#20-reconstruction-caveats-load-bearing-assumptions)
21. [Intentional oddities preserved from the binary](#21-intentional-oddities-preserved-from-the-binary)
22. [Verifying the reconstruction](#22-verifying-the-reconstruction)
23. [Security analysis](#23-security-analysis)
24. [Relationship to the 2019 iOS build](#24-relationship-to-the-2019-ios-build)
25. [Known issues / TODO](#25-known-issues--todo)
26. [Glossary](#26-glossary)
27. [References](#27-references)

---

## 1. What this is

This is the **first-stage boot image** for the Compact Pro (Android) LPC43xx part. It lives in external SPIFI flash mapped at `0x14000000` and its job is narrow:

1. Bring up the core and the SPI-NOR flash controller.
2. Migrate any firmware slot still in transport (Key-A) form to the device-storage (Key-B) form.
3. Pick one of three firmware "slots" in flash, based on a boot-config record (and an optional warm-boot rollback request).
4. Verify, decrypt, and segment-load the chosen firmware into internal SRAM, then jump to it.

The application image is protected with a **software xorshift128 stream cipher** keyed by a 128-bit value baked into the bootloader's own flash image. It is **not** authenticated encryption — there is no signature or MAC, only a 32-bit additive checksum. (See [§23](#23-security-analysis).)

This build is unusual in two respects that shape the whole repo: it runs its **entire boot pipeline from flash (XIP)** rather than relocating to SRAM, and it carries a **bespoke hand-written SPIFI driver** (not NXP lpcspifilib) which is the only thing relocated into RAM.

### What this is _not_

- It is **not** a clean-room rewrite. It is a behavioral reconstruction traced against the actual disassembly, byte-for-byte faithful on the crypto path and structurally faithful elsewhere.
- It is **not** a turnkey, flashable artifact. The imported SPIFI driver blob is treated as a black box (see [§6](#6-what-is-reconstructed-vs-imported), [§20](#20-reconstruction-caveats-load-bearing-assumptions)).
- It is **not** using the LPC43**S** hardware AES engine or OTP. The application is protected purely in software.

---

## 2. Security notice (read first)

> **The encryption keys in this repository are recovered constants, not secrets.**
>
> Both 128-bit keys are embedded in clear in the original flash image. Anyone with the dump already has them. They are reproduced here ([§16](#16-keys-and-where-they-live)) because they are required to understand and reproduce the loader's behavior, **not** because they protect anything.
>
> **The protection scheme is obfuscation, not security.** A non-linear PRNG keyed by fixed values, plus a 32-bit additive checksum, provides no authenticity guarantee. xorshift128 is fully invertible; a known-plaintext attack on the cleartext 64-byte header window (always present at image offset `0x200`) plus the predictable Cortex-M vector-table structure recovers keystream trivially. The two-key Key-A→Key-B migration is a key-management convenience, not a strengthening. Do **not** reuse this design where tamper-resistance matters. See [§23](#23-security-analysis).

---

## 3. At a glance

| Property               | Value                                                                              |
| ---------------------- | ---------------------------------------------------------------------------------- |
| Product                | Seek Thermal Compact Pro (Android variant), product code UQ-AAA                    |
| Internal build name    | _none embedded_ (marker `{01,02,00,00}` + build date/time only)                    |
| Build timestamp        | `Jun 26 2016  11:08:15` (at flash `0x14002958`)                                    |
| Image source           | 4 MB (32 Mbit) SPI-NOR dump (4,194,304 bytes); bootloader region is the low 64 KB  |
| Image SHA-256          | `db4efc84f5338815ef9e4fa8b8242d9d8fdfad7f118d97aaa0180cbbddae4dee`                 |
| Target                 | NXP LPC43xx, ARM Cortex-M4 (ARMv7-M), little-endian                                |
| Mapped base            | `0x14000000` (SPIFI memory-mapped external SPI-NOR)                                |
| Initial SP (MSP)       | `0x10018000`                                                                       |
| Reset vector           | `0x14000265` (Thumb), i.e. `Reset_Handler` at `0x14000264`                         |
| Application region     | `0x10000000`–`0x10010000` (64 KB)                                                  |
| Bootloader RAM runtime | `0x10010000`–`0x10018000` (32 KB: imported driver + data + bss + stack)            |
| Cipher                 | software xorshift128 (Marsaglia), shift triple 11/19/8                             |
| Key whitening          | **none** (seeds taken from key words directly)                                     |
| Acceptance test        | 32-bit decrypted-word checksum `== 0x00000000`                                     |
| Keys                   | two embedded (Key A = transport, Key B = at-rest) + optional silicon-derived Key B |
| Language               | C (GNU C99/C11)                                                                    |

For the specific dump this was reconstructed against, the boot decision resolves to: **slot A (`0x14050000`), Key B, segmented path, installed MSP `0x1000A000`, jump `0x100806A5`** — confirmed bit-exact in [§22](#22-verifying-the-reconstruction).

---

## 4. Target hardware

- **Product:** Seek Thermal Compact Pro (Android variant), product code UQ-AAA.
- **MCU:** NXP LPC43xx family (LPC4337-class). The loader places a 64 KB application at `0x10000000` plus its own 32 KB runtime above it, so the part has at least 96 KB of contiguous SRAM at `0x10000000` (the staging buffer at `0x20000000` is a separate AHB bank).
- **Core:** ARM Cortex-M4, ARMv7-M, little-endian, Thumb-2.
- **Boot source:** external SPI-NOR over the SPIFI controller (peripheral base `0x40003000`), memory-mapped (XIP) at `0x14000000`.
- **Flash device:** a 4 MB (32 Mbit) SPI-NOR part. The bespoke driver detects the device by JEDEC ID and carries per-vendor configuration paths (Spansion/Macronix/Winbond/ISSI/GigaDevice/SST/Micron — see the `spifi_cfg_*` routines in [§7](#7-manually-assigned-function-labels)).
- **SRAM regions used:** the low bank from `0x10000000` (application + bootloader runtime) and an AHB SRAM staging buffer at `0x20000000`.

---

## 5. Repository layout

```
CompactPro_43X0_Bootloader/                  ← workspace
│
├─ lpc_chip_43xx/                             ← LPCOpen chip library      [IMPORT, unchanged]
│  ├─ inc/   chip.h, cmsis.h, core_cm4.h,
│  │         scu_18xx_43xx.h, rgu_18xx_43xx.h, creg_18xx_43xx.h, …
│  └─ src/   (CMSIS + the few peripheral TUs actually linked)
│
├─ spifi_driver_blob/                         ← bespoke SPIFI driver      [IMPORT, black box]
│  └─ (relocated code+data blob; flash 0x14000BE8 → RAM 0x10010000, 0x1D70 bytes;
│      ~40 spifi_* / memcpy_fast routines; NOT NXP lpcspifilib)
│
└─ bootloader/                                ← THE APPLICATION           [RECONSTRUCT]
   ├─ inc/
   │   └─ bootloader.h          ← memory map, magic numbers, slot table, request ABI, prototypes
   ├─ src/
   │   ├─ startup_lpc43xx.c     ← g_pfnVectors[], the full monolithic Reset_Handler
   │   │                          (CRT0 + mailbox + spifi_init + A→B migration + slot
   │   │                          select + segmented load + handoff), spin fault handlers,
   │   │                          IRQ52_Handler catch-all, scatterload_copy_words
   │   ├─ boot_main.c           ← select_boot_slot, image_try_keys / _copy2,
   │   │                          image_checksum_ok / _copy2, memzero_words
   │   ├─ crypto_stream.c       ← prng_seed_from_key, stream_checksum16 / _copy2,
   │   │                          stream_reencrypt_keyA_to_keyB, stream_decrypt_skip_header_*,
   │   │                          stream_decrypt_segment (xorshift128 inlined)
   │   ├─ flash_if.c            ← spifi_init (pin-mux + request marshalling), flash_program,
   │   │                          flash_erase_region, flash_program_rmw, flash_chip_erase
   │   ├─ spifi_glue.c          ← imported-driver request ABI + context + the three thunks
   │   ├─ keys.c                ← g_key_mask (Key A), g_keyB, g_boot_config_ptr, g_build_info
   │   └─ util_mem.c            ← memcpy_auto, memcpy_bytes
   └─ ld/
       └─ lpc43xx_spifi_boot.ld ← 64 KB image @0x14000000; driver/.data/.bss relocated to
                                   0x10010000+; stack top 0x10018000
```

---

## 6. What is reconstructed vs. imported

The disassembly resolves on the order of **75 routines**:

- **The SPIFI driver portion is a bespoke blob and is imported, not rewritten.** This is the ~40 routines the analysis labelled `spifi_cmd*`, `spifi_make_cmd`, `spifi_write_enable` / `spifi_wren_then_cmd`, `spifi_enter_mem_mode` / `spifi_exit_mem_mode`, `spifi_read_sfdp` / `spifi_sum_sfdp_params` / `spifi_get_read_count`, `spifi_detect_4byte_addr`, `spifi_set_capacity`, `spifi_configure_modes`, `spifi_block_protect_engine`, `spifi_check_range`, `spifi_erase_cmd`, `spifi_program_setup` / `spifi_program_pages` / `spifi_program_region`, `spifi_find_nonff_word`, `spifi_addr_aligned`, `spifi_verify_equal` / `spifi_verify_erased`, `spifi_wait_program_done`, `spifi_write_status`, the per-vendor config paths `spifi_cfg_micron` / `_micron_2` / `_winbond_spansion` / `_read_cr` / `_sst` / `_macronix`, the `spifi_dummy_from_freq_*` timing helpers, plus `memcpy_fast` and `memcpy_bytes_thunk`. This driver is **not** NXP lpcspifilib — it pokes the SPIFI registers at `0x40003000` directly and switches on the JEDEC ID per vendor. It is treated as a black box: a relocated blob that gives you read/program/erase against `0x14000000`.

- **The bootloader-specific functions are reconstructed.** The startup/CRT0 and its fused boot pipeline (`Reset_Handler`), the spin fault/trap handlers, the boot leaves (`select_boot_slot`, `image_try_keys`, `image_try_keys_copy2`, `image_checksum_ok`, `image_checksum_ok_copy2`, `memzero_words`), the stream cipher (`prng_seed_from_key`, `stream_checksum16` / `_copy2`, `stream_reencrypt_keyA_to_keyB`, `stream_decrypt_skip_header_entry` / `_body`, `stream_decrypt_segment`), the flash wrapper layer (`spifi_init`, `flash_program`, `flash_erase_region`, `flash_program_rmw`, `flash_chip_erase`), the three driver thunks (`spifi_drv_init_thunk`, `spifi_drv_program_thunk`, `spifi_drv_op_thunk`), the small mem helpers (`memcpy_auto`, `memcpy_bytes`), and the embedded key/config data.

A practical consequence: **the bootloader-specific logic is the entire attack/patch surface.** Everything behind the three thunks is vendor plumbing.

---

## 7. Manually assigned function labels

The dump contains no function symbols. The labels below were assigned manually during analysis from each routine's behavior and call context, then kept consistent so the source compares directly with the listing. Addresses are flash/image addresses; the relocated driver routines map into RAM at `0x10010000` (see [§10](#10-memory-map)).

| Address      | Function label                     |     | Address      | Function label                |
| ------------ | ---------------------------------- | --- | ------------ | ----------------------------- |
| `0x14000260` | `IRQ52_Handler`                    |     | `0x14000c72` | `spifi_quad_opt_bits`         |
| `0x14000264` | `Reset_Handler`                    |     | `0x14000c82` | `spifi_cmd`                   |
| `0x14000564` | `NMI_Handler`                      |     | `0x14000ca4` | `spifi_cmd_addr`              |
| `0x14000568` | `HardFault_Handler`                |     | `0x14000cb6` | `spifi_cmd_data`              |
| `0x1400056c` | `MemManage_Handler`                |     | `0x14000ce2` | `spifi_write_enable`          |
| `0x14000570` | `BusFault_Handler`                 |     | `0x14000ce8` | `spifi_wren_then_cmd`         |
| `0x14000574` | `UsageFault_Handler`               |     | `0x14000d04` | `spifi_cmd_addr_data`         |
| `0x14000578` | `SVC_Handler`                      |     | `0x14000d4e` | `spifi_exit_mem_mode`         |
| `0x1400057c` | `DebugMon_Handler`                 |     | `0x14000d8e` | `spifi_wait_program_done`     |
| `0x14000580` | `PendSV_Handler`                   |     | `0x14000e68` | `spifi_write_status`          |
| `0x14000584` | `SysTick_Handler`                  |     | `0x14000e9a` | `spifi_enter_mem_mode`        |
| `0x1400058c` | `scatterload_copy_words`           |     | `0x14000f2a` | `spifi_detect_4byte_addr`     |
| `0x140005a8` | `memzero_words`                    |     | `0x14000f5c` | `spifi_sum_sfdp_params`       |
| `0x140005c0` | `image_try_keys` (Key A)           |     | `0x14000f98` | `spifi_get_read_count`        |
| `0x140005da` | `image_checksum_ok`                |     | `0x14000fb4` | `spifi_read_sfdp`             |
| `0x140005fc` | `image_try_keys_copy2` (Key B)     |     | `0x14001024` | `spifi_set_capacity`          |
| `0x14000616` | `image_checksum_ok_copy2`          |     | `0x1400103a` | `spifi_configure_modes`       |
| `0x1400063c` | `spifi_init`                       |     | `0x140013b8` | `spifi_block_protect_engine`  |
| `0x1400069c` | `flash_program`                    |     | `0x140017dc` | `spifi_check_range`           |
| `0x140006d4` | `flash_erase_region`               |     | `0x14001824` | `spifi_erase_cmd`             |
| `0x14000704` | `flash_program_rmw`                |     | `0x14001866` | `spifi_program_setup`         |
| `0x14000728` | `flash_chip_erase`                 |     | `0x1400187e` | `spifi_program_pages`         |
| `0x14000758` | `select_boot_slot`                 |     | `0x1400196e` | `spifi_find_nonff_word`       |
| `0x1400080c` | `memcpy_auto`                      |     | `0x14001986` | `spifi_addr_aligned`          |
| `0x14000844` | `memcpy_bytes`                     |     | `0x1400199c` | `spifi_verify_equal`          |
| `0x14000858` | `prng_seed_from_key`               |     | `0x140019fe` | `spifi_verify_erased`         |
| `0x140008c8` | `stream_checksum16` (Key A)        |     | `0x14001a7c` | `spifi_program_region`        |
| `0x1400093c` | `stream_reencrypt_keyA_to_keyB`    |     | `0x1400208c` | `spifi_cfg_micron`            |
| `0x140009f8` | `stream_decrypt_skip_header_entry` |     | `0x14002520` | `spifi_dummy_from_freq_a`     |
| `0x140009fe` | `stream_decrypt_skip_header_body`  |     | `0x14002552` | `spifi_cfg_winbond_spansion`  |
| `0x14000a70` | `stream_decrypt_segment`           |     | `0x14002698` | `spifi_cfg_read_cr`           |
| `0x14000b3c` | `stream_checksum16_copy2` (Key B)  |     | `0x140026da` | `spifi_cfg_micron_2`          |
| `0x14000bb8` | `spifi_drv_init_thunk`             |     | `0x14002784` | `spifi_cfg_sst`               |
| `0x14000bc8` | `spifi_drv_program_thunk`          |     | `0x1400283c` | `spifi_dummy_from_freq_b`     |
| `0x14000bd8` | `spifi_drv_op_thunk`               |     | `0x1400285a` | `spifi_cfg_macronix`          |
| `0x14000be8` | `memcpy_fast` (driver blob)        |     | `0x14002948` | `memcpy_bytes_thunk` (driver) |
| `0x14000c2a` | `spifi_cmd_read`                   |     |              |                               |
| `0x14000c5c` | `spifi_make_cmd`                   |     |              |                               |

> **Rename:** the disassembly labels `0x1400093c` as a third `stream_decrypt_skip_header`. It is renamed here to **`stream_reencrypt_keyA_to_keyB`** — it runs two keystreams in lockstep (Key A from the mask block, Key B via `prng_seed_from_key`) and emits `out = cipher ^ ksA ^ ksB`, converting a transport image to its at-rest form. See [§15](#15-cryptographic-scheme) and the file header in `crypto_stream.c`.

The driver-blob entries (`memcpy_fast` onward, from `0x14000be8`) are imported and not reproduced as source; they relocate into RAM at `0x10010000`. `memcpy_bytes_thunk` (`0x14002948`) is the one that relocates into RAM but branches **back** to `memcpy_bytes` in flash (`0x14000844`).

---

## 8. Build prerequisites

- **IDE:** LPCXpresso or its successor **MCUXpresso IDE**.
- **Toolchain:** GNU Arm Embedded (`arm-none-eabi-gcc`). C project; the "GNU C++" in the IDA header reflects only the managed-build linker driver.
- **NXP LPCOpen for LPC43xx:** the chip library only — CMSIS core headers, `chip.h`, and the SCU/RGU/CREG headers. The bootloader pokes SCU/CREG/RGU/NVIC directly, so a minimal subset compiles fine.
- **The bespoke SPIFI driver blob:** there is **no public source** for this driver (it is not lpcspifilib). To produce a runnable image you must supply the relocated blob bytes (flash `0x14000BE8`–`0x14002958`) or a compatible reimplementation behind the three-thunk + request-struct ABI in [§13](#13-image-format)/`spifi_glue.c`.

---

## 9. Building in MCUXpresso

Create the workspace tree from [§5](#5-repository-layout):

```
CompactPro_43X0_Bootloader/   (workspace)
├─ lpc_chip_43xx/      →  Static Library  (LPCOpen chip lib, imported)
├─ spifi_driver_blob/  →  Object/Library  (the bespoke driver, imported as a blob)
└─ bootloader/         →  Executable      (this reconstruction; references the above)
```

In the **bootloader** project properties:

- **MCU / linker** — select the LPC43xx part; turn the managed linker script **off** and point _Linker → Linker script_ at `bootloader/ld/lpc43xx_spifi_boot.ld` (the bootloader needs the custom flash-`@0x14000000` / SRAM-relocation layout).
- **Includes** — add `bootloader/inc` and the LPCOpen `inc` directories.
- **References** — add `lpc_chip_43xx` and the driver blob so they are linked.
- **Symbols** — define the LPCOpen part macros (`CORE_M4`, `CHIP_LPC43XX`).

**Toolchain flags:**

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mfpu=fpv4-sp-d16
-std=gnu11 -Os -ffunction-sections -fdata-sections -fno-strict-aliasing
# link:
-nostartfiles -Wl,--gc-sections
```

`-nostartfiles` because we supply `startup_lpc43xx.c`. `-fno-strict-aliasing` because the flash layer type-puns addresses freely. Unlike the 2019 iOS build, this `Reset_Handler` needs **no** `no-tree-loop-distribute-patterns` barrier: its scatter-load uses explicit flash-resident calls (`scatterload_copy_words`, `memzero_words`) rather than inline loops, so there is no memcpy/memset idiom for GCC to synthesize. See [§12](#12-boot-flow-end-to-end).

---

## 10. Memory map

### Flash (external SPI-NOR, memory-mapped at `0x14000000`)

| Region                       | Address                   | Notes                                                        |
| ---------------------------- | ------------------------- | ------------------------------------------------------------ |
| Bootloader image (this file) | `0x14000000`–`0x14010000` | 64 KB                                                        |
| SPIFI driver blob            | `0x14000BE8`–`0x14002958` | `0x1D70` bytes; relocated to RAM `0x10010000`                |
| `.data` (LMA)                | `0x1400297C`              | `0x160` bytes; copied to RAM `0x10011D70`                    |
| Build-info block             | `0x14002958`              | 4-byte marker + build date/time; the info pointer to the app |
| Key A / mask (LMA)           | `0x14002ABC`              | 16 bytes (`.data+0x140`)                                     |
| Key B fixed (LMA)            | `0x14002ACC`              | 16 bytes (`.data+0x150`)                                     |
| Boot-config record           | `0x14010000`              | 16 bytes; first dword selects the slot                       |
| Firmware slot A              | `0x14050000`              | encrypted application                                        |
| Firmware slot B              | `0x14060000`              | encrypted application                                        |
| Recovery / golden slot       | `0x14070000`              | encrypted application                                        |

Slots are `0x10000` (64 KB) apart. The boot-config base (`0x14010000`) is also handed to the booted application.

### RAM (the layout `Reset_Handler` establishes)

```
0x10000000 ┐ (decrypted app vector table lands here; 0x200/204/208 = handoff block;
           │  0x20C/210 = the warm-boot update mailbox, read at reset)
           │  64 KB application region
0x10010000 ┤ SPIFI driver blob   (RAM copy of flash 0x14000BE8+, 0x1D70 bytes)
0x10011D70 ┤ keys/config blob     (Key A @..EB0, Key B @..EC0; copied from 0x1400297C, 0x160)
0x10011ED0 ┤ BSS: SPIFI request struct (0x10011ED0) + driver context (0x10011EE4) — zeroed (0x94)
0x10017800 ┤ 0xCDCDCDCD stack guard  ← MSP − 0x800
0x10018000 ┘ MSP (bootloader stack top)
```

Staging buffer: `0x20000000` (a separate AHB SRAM bank) — used for program data marshalling and the read-modify-write block buffer.

### Flash ↔ RAM relocation formula (driver only)

`Reset_Handler` relocates flash `0x14000BE8 → RAM 0x10010000`. So for any RAM address `R` in the relocated driver:

```
flash = R − 0x10010000 + 0x14000BE8
```

This is how the three thunk targets (`0x100105FF`, `0x10010E95`, `0x100110EB`) and the cross-boundary `memcpy_bytes_thunk` map back to flash.

---

## 11. The build identity

This image embeds **no internal name string** (the 2019 iOS build embedded `80K_43X0_Bootloader`). The only identity baked in is a 4-byte marker `{01,02,00,00}` immediately followed by the build date/time `Jun 26 2016` / `11:08:15`, at flash `0x14002958`. The marker is read at runtime: `prng_seed_from_key` forms the big-endian composite `0x01020000` and compares it to `0x01010000` to choose the fixed-vs-silicon Key-B path (the composite exceeds the threshold, so the fixed key is used). The repository/workspace name `CompactPro_43X0_Bootloader` is therefore descriptive, anchored to the product code (UQ-AAA) and the build date. (`43X0` is an internal product/board designator carried over from the lineage; its precise meaning is not recoverable from the image alone.)

---

## 12. Boot flow, end to end

Traced against the analyzed dump, so the addresses and values are the real ones.

### Phase 0 — The part's boot ROM hands off

On power-up the LPC43xx internal boot ROM runs, strapped to boot from SPIFI. It maps the SPI-NOR at `0x14000000` and follows the Cortex-M reset convention:

- `MSP ← *(0x14000000) = 0x10018000`
- `PC  ← *(0x14000004) = 0x14000265` (Thumb), i.e. `Reset_Handler` at `0x14000264`, executing in place (XIP) from flash.

### Phase 1 — `Reset_Handler` runs the whole pipeline from flash

There is no separate RAM stage. `Reset_Handler` does, in order:

1. **CRT0 prologue.** `CPSID i`; `CREG_M4MEMMAP = 0x10000000` (alias the local SRAM bank to address 0); `VTOR = 0x14000000` (flash vector table for now). Plant the `0xCDCDCDCD` guard at `0x10017800` (`MSP−0x800`); re-set `MSP = 0x10018000`. Pulse-reset peripherals (`RGU_RESET_CTRL0 = 0x10DF1000`, `RGU_RESET_CTRL1 = 0x01DFF7FF`); clear all NVIC pending (`ICPR0..7`).
2. **Scatter-load** (three tables): copy `.data` (keys/config) `0x1400297C → 0x10011D70` (`0x160`); copy the SPIFI driver blob `0x14000BE8 → 0x10010000` (`0x1D70`); zero `.bss` `0x10011ED0` (`0x94`). The copies go through the flash-resident helper `scatterload_copy_words`; the zero through `memzero_words` — both flash-resident, so callable before the driver exists in RAM.
3. **Warm-boot update mailbox.** Read the flag at `0x1000020C` (honored only if it is `0xAA55FF01` or `0xAA55FF02`) and the 64-bit gate at `0x10000210` (must be `≤ 0x752F`); if both valid, derive a non-zero rollback request and clear the mailbox.
4. **`spifi_init`.** Pin-mux the SPIFI bus and bring the imported driver up into XIP through the init thunk.
5. **Key-A → Key-B migration** (every boot, all three slots). For each slot, if it validates under **Key A** (`image_try_keys` — still transport/OTA form): copy the ciphertext to the staging buffer, re-key it in place with `stream_reencrypt_keyA_to_keyB` (`out = cipher ^ ksA ^ ksB`, header window preserved), erase the slot's 64 KB block (`flash_chip_erase`), and write the Key-B image back (`flash_program`). Subsequent boots see a Key-B slot.
6. **`select_boot_slot`** with the rollback request from step 3 (`0` normally) — read the 16-byte config at `0x14010000` and resolve a slot via the Key-B gate `image_try_keys_copy2`.
7. **Segmented load (Key B).** Decrypt the `0x2C0`-byte segment table; load pass-1 (4 descriptors), zero 4 BSS regions, load pass-2 (4 descriptors). Each segment reseeds Key B and is fast-forwarded to its absolute position.
8. **Handoff.** Copy the first `0x200` bytes of the decrypted table to `0x10000000` (installs the app vector table); publish `0x10000200 = &build-info (0x14002958)`, `0x10000204 = config base (0x14010000)`, `0x10000208 = slot id (0=A,1=B,2=recovery)`; `VTOR = 0x10000000`; `CPSIE i`; jump the entry (segment-table word 132). Control never returns.

**Why a flash-resident driver-only relocation:** the migration and rollback paths erase and reprogram the SPIFI flash, which means leaving memory-mapped mode and driving the controller with explicit commands — you cannot fetch _driver_ instructions from flash while doing that. So only the driver is relocated to SRAM; the boot logic stays in flash and calls into the RAM driver through the three thunks when it needs command mode.

### Failure behavior

There is no rich error handling. A null slot base or a failed segment decrypt parks the core in a `while(1)` spin. Recovery is at the slot level (the A/B/recovery ordering). A corrupt image that still checksums correctly but misbehaves is the application's problem.

### One-line summary for this dump

```
ROM      : map SPIFI @0x14000000; MSP=0x10018000; jump 0x14000264
CRT0     : CPSID; M4MEMMAP=0x10000000; VTOR=0x14000000; canary@0x10017800; MSP=0x10018000
           RGU reset; clear NVIC pending
           copy .data ->0x10011D70; copy driver ->0x10010000; zero .bss @0x10011ED0
mailbox  : flag@0x1000020C / gate@0x10000210 -> rollback request (none here)
spifi    : pinmux P3_4..P3_8 (P3_3 unset, P3_4 written twice); driver up -> XIP (4 MB part)
migration: all slots checked under Key A -> none in transport form -> SKIPPED
select   : config selector blank -> slot A 0x14050000
load     : seed Key B; decrypt 0x2C0 table; 2 load passes + BSS zero
           pass-1 desc[0] = { srcRef 0x14009960, dstVMA 0x10000000, len 0x240 }
handoff  : install vec @0x10000000 (MSP 0x1000A000); 0x200/204/208; VTOR=0x10000000; CPSIE
           jump 0x100806A5 (table word 132)
app      : running from SRAM, owns 0x10000000..0x10018000
```

---

## 13. Image format

An application image is a stream of 32-bit words, encrypted with the positional keystream cipher, with one cleartext landmark. **There is no monolithic path and no "CODE" footer** (both present in the 2019 iOS build); the loader always takes the segmented path.

### Cleartext header window (always plaintext)

Words **128–143** (byte offset **`0x200`–`0x23F`**, 64 bytes) are stored **verbatim, not XORed** — the cleartext image header the loader reads before it knows the key:

- `header[0]` = magic `0xA1B2C3D4`
- `header[1]` = image length in bytes (must be `< 0x10000`)
- word 132 = the app entry pointer (used after the segment table is decrypted)

### Segment table

The loader decrypts a `0x2C0`-byte (176-word) table. The observed word offsets (the descriptor format is byte-confirmed for pass-1 against this dump; the BSS / pass-2 struct semantics are inferred by symmetry):

```
[128..143]  cleartext header window; word 132 = app entry pointer
[144..155]  pass-1 load descriptors: 4 × { srcRef, dstVMA, byteLen }
[156..163]  BSS descriptors:         4 × { addr, byteLen }
[164..175]  pass-2 load descriptors: 4 × { srcRef, dstVMA, byteLen }
```

`srcRef` is an image-base-relative address; the read offset within the slot is `srcRef − 0x14000000`, passed as `image_offset` to `stream_decrypt_segment` (which fast-forwards the keystream by `image_offset/4` words and keeps the 16-word window at `[skip_words..skip_words+15]`, `skip_words = 0x80`). On this dump the first pass-1 descriptor is `{ 0x14009960, 0x10000000, 0x240 }` — i.e. read offset `0x9960`, load to the app vector base, length `0x240`.

### SPIFI driver request ABI (program/erase)

The flash layer marshals one operation into a fixed struct (instance at `0x10011ED0`) and calls a thunk:

```
+0x00 flash_offset : target byte offset from 0x14000000
+0x04 length       : byte count
+0x08 stage_buf    : AHB staging buffer (0x20000000) for program data, or 0 for erase
+0x0C sentinel     : 0 / 0xFFFFFFFF
+0x10 opcode       : 0x08 = program, 0x20 = erase
```

The request ABI carries **no source pointer**, so program data is staged through `0x20000000` first; reads are plain XIP loads.

---

## 14. The slot system

Three firmware slots plus a 16-byte config record drive selection. `select_boot_slot(update_flag)` reads the record at `0x14010000` and returns the chosen slot base.

### Selection logic (`update_flag == 0`, the normal call)

The selector dword (`cfg[0]`) sets the preference order; each candidate is gated through the **Key-B** acceptance test `image_try_keys_copy2`:

- **blank** (`0x00000000` or `0xFFFFFFFF`) → try **A**, then **B**, then **recovery**
- **`1`** → prefer **B**, then **A**, then **recovery**
- **any other value** (e.g. `2`) → prefer **recovery**, then **A**, then **B**

The predicate is `(uint32_t)(cfg[0]-1) <= 0xFFFFFFFD`, false only for the two blank values — that is the whole "blank → prefer A" branch. (On this dump the record is all `0xFF`, so the blank branch is taken and slot A wins.)

### Rollback path (`update_flag != 0`)

Driven by the warm-boot mailbox. It re-resolves the current slot, and if it is A or B, stamps the flag over that slot's header magic (invalidating it), forces the config selector to `2` via the read-modify-write path (`flash_program_rmw`, which preserves the rest of the config block), and returns recovery.

---

## 15. Cryptographic scheme

### Cipher — xorshift128 (Marsaglia)

`xorshift128_next` is the textbook 32-bit xorshift128 with shift constants **11 / 19 / 8**, state `[x, y, z, w]`:

```
t = x ^ (x << 11)
x = y;  y = z;  z = w
w = w ^ (w >> 19) ^ t ^ (t >> 8)
return w                       ; one keystream word
```

There is no standalone PRNG routine; this step is inlined into every stream routine.

### Key → seed transform

The 16-byte key (`k0,k1,k2,k3` as little-endian dwords) is rotated by one word into the state, with **no whitening**:

```
x = k1
y = k2
z = k3
w = k0          ; key word 0 lands in w, not x
```

(The 2019 iOS build XORed a fixed mask `0x13579BDF` into each of these; this image does not.)

### Two keys and the migration

- **Key A** (transport/OTA): seeded directly from the 16-byte mask block. A slot in transport form validates only under Key A.
- **Key B** (at-rest device): seeded by `prng_seed_from_key`, which reads the build marker and uses either the **fixed** embedded Key B (when the composite marker `> 0x01010000`, as here) or a **silicon-derived** Key B = four device-ID dwords at `0x40045000` XORed with the mask block.
- **Migration** (`stream_reencrypt_keyA_to_keyB`): runs both generators in lockstep and emits `out = cipher ^ ksA ^ ksB`, converting a Key-A image into its Key-B form, header window preserved. Run at boot over all slots so that storage settles on Key B.

### Decryption — positional XOR

Each ciphertext word is XORed with one keystream word; the PRNG is advanced for _every_ word (so keystream word N lines up with image word N), including the verbatim header window. `stream_decrypt_segment` fast-forwards by `image_offset/4` words so the keystream aligns to each segment's position.

### Integrity check

`stream_checksum16` / `stream_checksum16_copy2` decrypt and sum all plaintext words into a 32-bit accumulator; the image is accepted only if that accumulator equals exactly **`0x00000000`** (the 2019 iOS build used `0x0000FFFF`). Each routine returns **two** sums through out-params — a raw-ciphertext sum (vestigial) and the decrypted sum; only the decrypted sum is checked. `_copy` keys Key A; `_copy2` keys Key B. The acceptance decision is the checksum alone (no decrypted-SP range check). For this dump, slot A sums to `0x1349A524` under Key A (reject) and exactly `0x00000000` under Key B (accept) — see [§22](#22-verifying-the-reconstruction).

---

## 16. Keys and where they live

Two embedded 128-bit keys, plus an optional silicon-derived Key B.

### Key A — the mask block (transport key, and the silicon-derivation mask)

```
RAM (runtime) : 0x10011EB0      Flash : 0x14002ABC      File offset : 0x2ABC   (.data+0x140)
Bytes (16)    : 67 A3 EA 21 82 4F EC C4 B3 C3 B0 A8 DA 51 46 69
```

Seed words (little-endian dwords): `k0=0x21EAA367`, `k1=0xC4EC4F82`, `k2=0xA8B0C3B3`, `k3=0x694651DA` → state `[k1,k2,k3,k0] = [0xC4EC4F82, 0xA8B0C3B3, 0x694651DA, 0x21EAA367]` (no whitening).

### Key B — fixed (at-rest device key)

```
RAM (runtime) : 0x10011EC0      Flash : 0x14002ACC      File offset : 0x2ACC   (.data+0x150)
Bytes (16)    : FA AC B3 C6 F1 41 24 69 BD 12 2F B8 2D 78 16 0D
```

Seed words: `k0=0xC6B3ACFA`, `k1=0x692441F1`, `k2=0xB82F12BD`, `k3=0x0D16782D` → state `[k1,k2,k3,k0] = [0x692441F1, 0xB82F12BD, 0x0D16782D, 0xC6B3ACFA]`.

### Silicon-derived Key B (not used in this dump)

When the build marker does **not** exceed `0x01010000`, Key B is derived from four device-unique dwords at `0x40045000` XORed with the mask block. This dump's marker is `0x01020000`, so the fixed key above is active.

### How the keys get into RAM

`Reset_Handler` copies `0x160` bytes from flash `0x1400297C → 0x10011D70`. That blob holds the config base (`0x14010000`), then the mask/Key-A block (`0x10011EB0`), then the fixed Key B (`0x10011EC0`).

To decrypt a captured image: seed xorshift128 with the matching key's `[x,y,z,w]`, keep words 128–143 verbatim, and XOR every other 32-bit word with one keystream word.

---

## 17. Module reference

### `bootloader.h`

The single shared header: flash/RAM memory map, image-format magic numbers and offsets, slot bases, the SPIFI request ABI, return-code legend, build-marker thresholds, and prototypes for every reconstructed function. Pointers are real pointers; argument order/semantics preserved; the gotcha functions carry warning comments.

### `startup_lpc43xx.c`

`g_pfnVectors[]` and the fused `Reset_Handler` — CRT0 (vector remap, peripheral reset, NVIC clear, scatter-load via `scatterload_copy_words` + `memzero_words`), the warm-boot mailbox, `spifi_init`, the Key-A→Key-B migration sweep, `select_boot_slot`, the segmented decrypt/load, and the handoff. Plus the plain-spin core-fault handlers, the `IRQ52_Handler` catch-all (no live peripheral handlers in this image), and `scatterload_copy_words`. No relocation-barrier is needed (the copies are explicit flash-resident calls).

### `boot_main.c`

The boot leaves: `select_boot_slot`, the two single-key gates `image_try_keys` (Key A) / `image_try_keys_copy2` (Key B), their checksum tails `image_checksum_ok` / `_copy2`, and `memzero_words`. The acceptance test is the keystream checksum alone (`== 0`). All flash-resident.

### `crypto_stream.c`

The keystream cipher: `prng_seed_from_key` (Key-B seeder with the fixed/silicon branch), `stream_checksum16` / `_copy2`, `stream_reencrypt_keyA_to_keyB` (the dual-key migration transform), `stream_decrypt_skip_header_entry` / `_body`, `stream_decrypt_segment`. Verified byte-for-byte against the dump (no-whitening seed, 11/19/8 shifts, verbatim window, 32-bit sentinel-0 checksum). The checksum and decrypt loops are deliberately the same loop differing only by store-vs-accumulate.

### `flash_if.c`

The flash-access layer: `spifi_init` (pin-mux + request marshalling), `flash_program`, `flash_erase_region`, `flash_program_rmw`, `flash_chip_erase`. Program data is staged through `0x20000000`; reads are XIP. **No** read-back verify on program and **no** blank-verify on erase. All real flash work goes through the three thunks.

### `spifi_glue.c`

The imported-driver boundary: the request-struct ABI, the request/context BSS objects, and the three flash-resident thunks (`spifi_drv_init_thunk` / `_program_thunk` / `_op_thunk`) that branch into the relocated driver at `0x10010000`. The driver bytes themselves are imported. Documents the relocation formula and the cross-boundary `memcpy_bytes_thunk`.

### `keys.c`

The embedded key material and config base: `g_key_mask` (Key A and the silicon mask), `g_keyB` (fixed Key B), `g_boot_config_ptr` (`0x14010000`), and the flash-resident `g_build_info` block (marker + build date/time, the info pointer handed to the app).

### `util_mem.c`

`memcpy_auto` (word copy + byte tail — the workhorse for header reads, staging, the vector-table install) and `memcpy_bytes` (plain byte copy, also the branch target of the driver's relocated `memcpy_bytes_thunk`). The driver's own `memcpy_fast` lives in the imported blob.

### `ld/lpc43xx_spifi_boot.ld`

The custom layout: vectors at `0x14000000`, the flash-resident `.text_boot`, the scatter-load section table (copy `.data`, copy the driver `.text_ram`, zero `.bss`), the driver `.text_ram` relocated to `0x10010000`, `.data` to `0x10011D70`, `.bss` (NOLOAD) at `0x10011ED0`, and stack top `0x10018000`.

---

## 18. Configuration constants

The key tunables (all in `bootloader.h`):

```c
#define SPIFI_XIP_BASE        0x14000000u   /* external SPI-NOR window         */
#define BOOT_CONFIG_BASE      0x14010000u   /* 16-byte boot-config record      */
#define SLOT_A_BASE           0x14050000u
#define SLOT_B_BASE           0x14060000u
#define SLOT_RECOVERY_BASE    0x14070000u
#define SLOT_STRIDE           0x00010000u   /* 64 KB between slots             */
#define SPIFI_CTRL_BASE       0x40003000u
#define SILICON_ID_BASE       0x40045000u   /* device-ID words (silicon Key B) */
#define RAM_APP_LOAD_BASE     0x10000000u   /* decrypted app vectors land here */
#define RAM_HANDOFF_BLOCK     0x10000200u   /* +0 info ptr, +4 cfg ptr, +8 slot id */
#define RAM_UPDATE_FLAG       0x1000020Cu   /* warm-boot mailbox flag          */
#define RAM_UPDATE_MAGIC      0x10000210u   /* warm-boot mailbox 64-bit gate   */
#define RAM_DRIVER_BASE       0x10010000u   /* relocated SPIFI driver          */
#define RAM_REQUEST_STRUCT    0x10011ED0u   /* SPIFI request struct            */
#define RAM_DRIVER_CONTEXT    0x10011EE4u   /* SPIFI driver context            */
#define MSP_TOP               0x10018000u
#define STAGING_BUF_BASE      0x20000000u
#define IMG_HEADER_OFFSET     0x200u        /* cleartext header @ slot+0x200   */
#define IMG_MAGIC             0xA1B2C3D4u
#define IMG_MAX_LEN           0x10000u      /* length cap                      */
#define SEG_TABLE_BYTES       0x2C0u
#define IMG_HDR_WORD_FIRST    128u          /* 0x200 / 4 (verbatim window)     */
#define IMG_HDR_WORD_LAST     143u
#define SEG_SKIP_WORDS        0x80u
#define ACCEPT_SENTINEL       0x00000000u   /* decrypted checksum must equal this */
#define BUILD_MARKER          0x01020000u   /* this image's marker composite   */
#define BUILD_MARKER_FIXEDKEY 0x01010000u   /* > this -> fixed Key B           */
#define UPDATE_FLAG_A         0xAA55FF01u
#define UPDATE_FLAG_B         0xAA55FF02u
#define UPDATE_MAGIC_GATE     0x752Fu
#define SLOTID_A              0
#define SLOTID_B              1
#define SLOTID_RECOVERY       2
#define SPIFI_OP_PROGRAM      0x08u
#define SPIFI_OP_ERASE        0x20u
#define SPIFI_STAGE_FLAG      0x20000000u
#define FL_OK                 0
#define FL_BADARG             11            /* 0xB */
#define FL_NOTINIT            2
```

---

## 19. Naming conventions and known misnomers

The manually assigned labels are preserved verbatim so source and listing line up 1:1. Several are **misleading** and are flagged in the code:

| Function                                  | Name suggests                | Actually does                                                                                                                               |
| ----------------------------------------- | ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `stream_decrypt_skip_header` (0x1400093c) | a third single-key decryptor | runs **two** keystreams (`out = cipher ^ ksA ^ ksB`) to convert Key-A → Key-B form — **renamed** here `stream_reencrypt_keyA_to_keyB`       |
| `stream_checksum16` / `_copy2`            | a 16-bit / mod-2¹⁶ sum       | accumulates and compares the **full 32-bit** decrypted-word sum against `0x00000000`; also returns a vestigial raw sum through an out-param |
| `image_try_keys` / `_copy2`               | "tries keys" (plural)        | each tries exactly **one** key — `image_try_keys` = Key A (transport detect), `_copy2` = Key B (boot gate)                                  |
| `prng_seed_from_key`                      | a generic key seeder         | seeds **only** the Key-B / silicon-derived path; the Key-A seed is inlined at its call sites                                                |
| `flash_chip_erase`                        | erases the whole device      | erases the **single 64 KB block** containing `addr` (fixed `0x10000` length, no blank-verify)                                               |

---

## 20. Reconstruction caveats (load-bearing assumptions)

These assumptions are load-bearing — losing them will break a build or mislead an analyst. The crypto path, scatter tables, key/marker constants, and slot geometry are **byte-confirmed** against the dump (see [§22](#22-verifying-the-reconstruction)); what remains inferred:

- **The SPIFI driver blob is imported, not sourced.** It is **not** lpcspifilib — it is a bespoke driver that pokes the SPIFI registers at `0x40003000` and switches per vendor by JEDEC ID. The reconstruction reproduces only its **call surface** (the three-thunk + request-struct ABI in `spifi_glue.c`/`flash_if.c`); the driver bytes (flash `0x14000BE8`–`0x14002958`) must be supplied to build a runnable image.
- **The request-struct field map** (`flash_offset/length/stage_buf/sentinel/opcode` at `+0x00/04/08/0C/10`) and the three thunk entry points (`0x100105FF` init, `0x10010E95` program, `0x100110EB` op/erase) are read off the call sites; confirm them against the blob you vendor.
- **Segmented-path table offsets** — pass-1's `{srcRef, dstVMA, byteLen}` descriptor format is byte-confirmed on this dump (`{0x14009960, 0x10000000, 0x240}`); the BSS `[156..]` and pass-2 `[164..]` struct semantics are **inferred** by symmetry.
- **The warm-boot mailbox flag→request mapping** (`0xAA55FF01 → 1`, `0xAA55FF02 → 2`) is inferred; the gate predicates (`flag ∈ {…}`, `gate ≤ 0x752F`) are confirmed.
- **The RGU reset masks** (`0x10DF1000` / `0x01DFF7FF`) match the 2019 iOS build's sequence; if this revision differs, only those two literals change.
- **The silicon-Key-B path** (`0x40045000` device-ID XOR mask) is reconstructed but **unexercised** by this dump (its marker selects the fixed key).

---

## 21. Intentional oddities preserved from the binary

These look like bugs but are faithfully reproduced because they are in the original machine code:

- **`spifi_init` skips P3_3 and writes P3_4 twice** — the pin-mux configures `P3_4..P3_8` (the 2019 iOS build set `P3_3..P3_8`), and `P3_4` is written `0xF3` then immediately overwritten with `0xD3`. Both are reproduced verbatim.
- **`flash_chip_erase` erases one fixed 64 KB block** despite its name, and performs no blank-verify (and the program/erase paths perform no read-back verify).
- **The checksum routines return a vestigial raw-ciphertext sum** through a second out-param that no caller reads.
- **`memcpy_bytes_thunk` is a RAM→flash cross-boundary call** — it relocates into SRAM with the driver blob but branches back to `memcpy_bytes` in flash (`0x14000844`).
- **Storage is left re-encrypted, not plaintext** — the Key-A→Key-B migration writes a _re-keyed_ image back to flash, so a migrated slot is still encrypted (under Key B), just under a different key than it arrived in.

---

## 22. Verifying the reconstruction

You do **not** need silicon to check the logic — the crypto path (`crypto_stream.c` + the seeders) is self-contained and host-compilable. It has been **verified bit-exact against this dump** (slot A, length `0xCA88`):

```
stream_checksum16(slot, 0xCA88, seed(KeyA))         -> Σdec = 0x1349A524   (Key A rejects)
stream_checksum16_copy2(slot, 0xCA88, seed(KeyB))   -> Σdec = 0x00000000   (Key B accepts)
migration sweep: no slot validates under Key A      -> SKIPPED
select_boot_slot(0): config selector blank (all 0xFF) -> slot A (0x14050000)
segment-table decrypt (Key B), then load:
  installed app vector[0] (MSP) = 0x1000A000
  jump entry = segment-table word 132 = 0x100806A5
  pass-1 descriptor[0] = { srcRef 0x14009960, dstVMA 0x10000000, len 0x240 }
```

Image identity: 4,194,304 bytes, SHA-256 `db4efc84f5338815ef9e4fa8b8242d9d8fdfad7f118d97aaa0180cbbddae4dee`. Because the checksum reaches exactly `0x00000000` and the decrypted vector table yields the expected MSP/entry, the cipher reconstruction is confirmed correct; the SPIFI half is then the imported driver doing read/program/erase against `0x14000000`.

---

## 23. Security analysis

- **Software xorshift, not hardware AES.** Although LPC43**S** parts include an AES engine + OTP, this bootloader protects the application with a software xorshift128 keystream. There is no authentication — no signature or MAC — only a 32-bit additive checksum.
- **Acceptance is the checksum alone.** A slot is accepted purely on its plaintext checksum reaching `0x00000000` under a key; there is no decrypted entry/SP range check.
- **Keys are in the clear in flash.** Both 128-bit keys are constants in the image; anyone with the dump has them. The silicon-derived Key-B path (device-ID XOR mask) is only as strong as the secrecy of the mask, which is also in the image.
- **Known-plaintext is fatal.** xorshift128 is fully invertible: ~16 bytes of known plaintext at a known offset recovers part of the state, and the cleartext 64-byte header window (offset `0x200`) plus the standard Cortex-M vector-table structure provide predictable plaintext to attack. The two-key migration does not change this — both keystreams are recoverable.
- **The checksum is trivially forgeable.** A 32-bit additive sum imposes no meaningful integrity barrier against an attacker who controls the plaintext.

**Bottom line:** this scheme is symmetric obfuscation. It raises the bar against casual cloning of an encrypted image, and nothing more. Do not adopt it where a motivated attacker has the bootloader image.

---

## 24. Relationship to the 2019 iOS build

A closely related LPC43xx bootloader ships on the Compact Pro **iOS** variant, built `Jan 7 2019` (marker `{C9,12,03,00}`, internal name `80K_43X0_Bootloader`). It is the same product family and toolchain lineage, and the two share the magic/segment-table format, the xorshift128 algorithm family, the slot/config model, and the overall boot intent.

The substantive differences below track the **~2.5-year build-date gap** (`Jun 26 2016` here vs `Jan 7 2019` there) — they are firmware-lineage evolution, **not** a consequence of the host OS. The bootloader does not care which phone OS the device pairs with; "iOS" and "Android" here are just the two captured builds, and the older one is simply structured differently. The load-bearing differences, for anyone cross-referencing the two:

- **Runs from flash, not RAM.** This (2016) image executes its entire boot pipeline in place (XIP) inside `Reset_Handler`; the 2019 build relocated all of its code to SRAM and ran a separate `boot_main` stage there. Here, only the SPIFI driver blob is relocated.
- **Bespoke SPIFI driver, not lpcspifilib.** This image carries a hand-written register-level driver (imported as a blob, reached through three thunks); the 2019 build wrapped NXP lpcspifilib.
- **Two keys + a boot-time A→B migration.** The 2019 build had a single key path (Key B / optional per-device slot); this image distinguishes a transport Key A from an at-rest Key B and re-encrypts slots at boot.
- **No whitening, sentinel 0.** Seeds are taken from the key words directly (the 2019 build XORed `0x13579BDF`); the checksum sentinel is `0x00000000` (the 2019 build used `0x0000FFFF`).
- **Segmented-only.** No monolithic path and no "CODE" footer (the 2019 build had both).
- **All-trap vector table.** No live peripheral IRQ handlers (the 2019 build installed three on IRQ12/13/14); core faults spin with a plain `B .` rather than `WFI`.
- **Different geometry and identity.** Initial SP `0x10018000` (2019 build `0x10020000`); 64 KB slots at `0x14050000`/`0x14060000`/`0x14070000`, stride `0x10000` (2019 build `0x20000`); marker `{01,02,00,00}` / `Jun 26 2016` and **no** internal name string (2019 build `{C9,12,03,00}` / `Jan 7 2019` / `80K_43X0_Bootloader`).

This section is for cross-reference only; the rest of this document stands on its own against the present image.

---

## 25. Known issues / TODO

- **Runnability:** supply the bespoke SPIFI driver blob (or a reimplementation behind the three-thunk + request-struct ABI) before expecting program/erase on hardware.
- **Driver ABI pinning:** confirm the request-struct field offsets and the three thunk entry points against the blob revision you vendor.
- **Silicon Key-B path:** unexercised by this dump (the marker selects the fixed key); validate the `0x40045000` device-ID derivation against a unit that uses it, if one is captured.
- **Segmented table layout:** pass-1 is byte-confirmed; the inferred BSS / pass-2 descriptor semantics deserve validation against a second sample.
- **Mailbox flag→request mapping:** the `0xAA55FF01→1 / 0xAA55FF02→2` derivation is inferred; confirm against a unit that exercises the warm-boot rollback.

Potential follow-up artifacts:

- A companion **host-side encryptor** that round-trips images for either key, including the A→B migration transform.
- A symbolized export of the imported driver blob so the `spifi_*` device-config paths read as source.

---

## 26. Glossary

- **SPIFI** — NXP's SPI Flash Interface peripheral; supports memory-mapped (XIP) reads from external SPI-NOR.
- **XIP (eXecute In Place)** — running/reading code directly from memory-mapped flash without copying to RAM.
- **CRT0** — the C runtime startup that initializes `.data`/`.bss` and hands control to the program.
- **VTOR** — Vector Table Offset Register (`0xE000ED08`); points the core at the active exception vector table.
- **SFDP / JEDEC ID** — standard SPI-NOR discovery (`0x5A` read-SFDP) and identity (`0x9F` read-ID) commands the bespoke driver uses to detect the device and pick a vendor config path.
- **Transport vs. at-rest key** — Key A protects an image in transit/OTA; the boot-time migration re-keys it to Key B for storage on the device.
- **Verbatim window** — the 16-word (`0x200`–`0x23F`) cleartext header region the cipher copies unchanged.

---

## 27. References

- **NXP LPC43xx User Manual (UM10503)** — SPIFI, CREG (`M4MEMMAP`), RGU, SCU, NVIC register details.
- **ARMv7-M Architecture Reference Manual** — Cortex-M4 reset behavior, vector table, VTOR, `MSP`.
- **NXP LPCOpen** — chip layer (CMSIS + peripheral drivers) for LPC43xx.
- **JEDEC JESD216 (SFDP)** — Serial Flash Discoverable Parameters, used by the bespoke driver's device detection.
- **Marsaglia, G., "Xorshift RNGs," Journal of Statistical Software (2003)** — the PRNG family used as the keystream generator (shift triple 11/19/8).

---

_This README documents a reverse-engineering reconstruction. The disassembly is the source of truth; where this document and the listing disagree, trust the listing. The embedded keys are recovered constants, not secrets._
