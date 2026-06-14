# Seek Thermal Nano 300 Bootloader

**A reverse-engineered and reconstructed first-stage secure bootloader for the Seek Thermal Nano 300, built on NXP LPC43xx (ARM Cortex-M4) and running from external SPIFI flash.**

This repository contains a from-scratch C reconstruction of the Seek Thermal Nano 300 LPC43xx boot image, recovered by disassembling a 4 MB SPI-NOR flash dump. The reconstruction is faithful to the original machine code (the disassembly is the source of truth). The flash dump contains no function symbols; the labels used here were assigned manually during analysis based on behavior and call context, then kept consistent so the source lines up 1:1 with the listing.

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
11. [The "80K" in the name](#11-the-80k-in-the-name)
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
24. [Known issues / TODO](#24-known-issues--todo)
25. [Glossary](#25-glossary)
26. [References](#26-references)

---

## 1. What this is

This is the **first-stage boot image** for the Seek Thermal Nano 300's LPC43xx part. It lives in external SPIFI flash mapped at `0x14000000` and its job is narrow and specific:

1. Bring up the core and the SPI-NOR flash controller.
2. Pick one of several firmware "slots" in flash, based on a boot-config record.
3. Verify, decrypt, and load the chosen firmware image into internal SRAM, then jump to it.

The application image is protected with a **software xorshift128 stream cipher** keyed by a 128-bit value baked into the bootloader's own flash image. It is **not** authenticated encryption — there is no signature or MAC, only a 16-bit additive checksum and a stack-pointer range check. (See [§23 Security analysis](#23-security-analysis).)

The original was built **Aug 12 2021** under an LPCXpresso/MCUXpresso managed build using the GNU Arm Embedded toolchain, with NXP's **LPCOpen** chip layer and **lpcspifilib** SPI-NOR driver. This repository reconstructs the bootloader-specific logic in idiomatic C and **imports** the linked lpcspifilib routines unchanged.

### What this is _not_

- It is **not** a clean-room or "inspired by" rewrite. It is a behavioral reconstruction traced against the actual disassembly, intended to be byte-for-byte faithful on the crypto path and structurally faithful elsewhere.
- It is **not** a turnkey, flashable artifact out of the box. At least one runtime dependency (the RMW staging buffer pointer at `0x20000000`) is populated by board/SDK bring-up code that is **outside** the 64 KB image and is therefore not reproduced here. See [§20](#20-reconstruction-caveats-load-bearing-assumptions).
- It is **not** using the LPC43**S** hardware AES engine or OTP. Despite the silicon supporting it, this bootloader protects the application purely in software.

---

## 2. Security notice (read first)

> **The encryption keys in this repository are recovered constants, not secrets.**
>
> Both 128-bit keys are embedded in clear in the original flash image. Anyone with the dump already has them. They are reproduced here ([§16](#16-keys-and-where-they-live)) because they are required to understand and reproduce the loader's behavior, **not** because they protect anything.
>
> **The protection scheme is obfuscation, not security.** A non-linear PRNG keyed by a fixed value, plus a 16-bit additive checksum and an address sanity check, provides no authenticity guarantee. xorshift128 is fully invertible; a known-plaintext attack on the cleartext 64-byte header window (always present at image offset `0x200`) plus the predictable Cortex-M vector-table structure recovers keystream trivially. Do **not** reuse this design for anything that needs to resist tampering. See [§23](#23-security-analysis) for the full breakdown.

---

## 3. At a glance

| Property                 | Value                                                              |
| ------------------------ | ------------------------------------------------------------------ |
| Product                  | Seek Thermal Nano 300                                              |
| Internal build name      | `80K_4320_Bootloader`                                              |
| Build timestamp          | `Aug 12 2021 08:20:23`                                             |
| Image size               | 65,536 bytes (`0x10000`)                                           |
| Image SHA-256            | `B03ED8F6221411307F3CE57AAEE76218F7AEB2A1451426113922CEAC348D0A87` |
| Target                   | NXP LPC43xx, ARM Cortex-M4 (ARMv7-M), little-endian                |
| Mapped base              | `0x14000000` (SPIFI memory-mapped external SPI-NOR)                |
| Initial SP (MSP)         | `0x10018000`                                                       |
| Reset vector             | `0x14000371` (Thumb)                                               |
| Cipher                   | software xorshift128 (Marsaglia), shift triple 11/19/8             |
| Key whitening mask       | `0x13579BDF`                                                       |
| Acceptance test          | full 32-bit plaintext checksum `== 0x0000FFFF`                     |
| Total functions analyzed | 109 (custom bootloader code plus imported lpcspifilib routines)    |
| Language                 | C (GNU C99/C11)                                                    |

For the specific dump this was reconstructed against, the boot decision resolves to: **slot A (`0x14030000`), Key B, monolithic path, MSP `0x10017800`, jump `0x10000409`** — see [§22 Verifying the reconstruction](#22-verifying-the-reconstruction).

---

## 4. Target hardware

- **Product:** Seek Thermal Nano 300.
- **MCU:** NXP LPC43xx family (the reconstruction matches an LPC4337-class part; `is_allowed_entry_addr` validates two SRAM windows, `0x10000000` and `0x10080000`, consistent with the chip's split SRAM banks).
- **Core:** ARM Cortex-M4, ARMv7-M, little-endian, Thumb-2.
- **Boot source:** external SPI-NOR over the SPIFI controller (peripheral base `0x40003000`), memory-mapped (XIP) at `0x14000000`.
- **Flash device in the analyzed dump:** a 4 MB (32 Mbit) SPI-NOR part. lpcspifilib's device database supports Winbond `W25Q80BV/64FV/32FV`, Spansion `S25FL512S/256S/164K/129P/064P/032P/016K`, and Macronix `MX25L8035E/6435E/3235E/1635E`; the attached part is detected at runtime by JEDEC ID.
- **SRAM regions used:** low bank from `0x10000000` (application + bootloader runtime), with a second bank window at `0x10080000` accepted by the entry-address check.

---

## 5. Repository layout

```
80K_4320_Bootloader/                         ← workspace
│
├─ lpc_chip_43xx/                             ← LPCOpen chip library      [IMPORT, unchanged]
│  ├─ inc/   chip.h, cmsis.h, core_cm4.h,
│  │         scu_18xx_43xx.h, rgu_18xx_43xx.h, creg_18xx_43xx.h, …
│  └─ src/   (CMSIS + the few peripheral TUs actually linked)
│
├─ lpcspifilib/                               ← NXP lpcspifilib            [IMPORT, unchanged]
│  ├─ inc/
│  │   ├─ spifilib_api.h        ← public API (spifiInit, spifiProgram, spifiRead, …)
│  │   └─ spifilib_dev.h        ← handle + family-descriptor internals
│  └─ src/
│      ├─ spifilib_dev.c                     ← core driver + HW register layer
│      └─ spifilib_fam_standard_cmd_set.c    ← "Common SPIFI Command Set" family + device DB
│
└─ bootloader/                                ← THE APPLICATION           [RECONSTRUCT]
   ├─ inc/
   │   └─ bootloader.h          ← memory map, magic numbers, slot table, prototypes
   ├─ src/
   │   ├─ startup_lpc43xx.c     ← g_pfnVectors[], Reset_Handler, fault handlers
   │   ├─ boot_main.c           ← boot_main, select_boot_slot, image_try_keys,
   │   │                           is_allowed_entry_addr, memzero_words
   │   ├─ crypto_stream.c       ← xorshift128_next, prng_seed_*, stream_decrypt_*,
   │   │                           stream_checksum16, stream_decrypt_segment
   │   ├─ spifi_glue.c          ← spifi_init, spifi_deinit, spifi_lock_acquire/release
   │   │                           (bootloader wrappers over lpcspifilib + one handle)
   │   ├─ flash_if.c            ← mem_read(_any), mem_cmp_any, mem_is_filled,
   │   │                           flash_program(_rmw), flash_chip_erase,
   │   │                           buf_all_eq, dwords_all_eq
   │   ├─ util_mem.c            ← memcpy_words/halfwords/auto, memset_bytes,
   │   │                           memcmp_bytes, memcpy_fast
   │   └─ keys.c                ← g_boot_config_base, g_keyA, g_keyB,
   │                              g_device_key_slot_flash (linker-placed)
   └─ ld/
       └─ lpc43xx_spifi_boot.ld ← 64 KB image @0x14000000, RAM relocation to
                                   0x10014000, device-key slot fixed @0x14000218
```

---

## 6. What is reconstructed vs. imported

Of the 109 functions resolved in the disassembly:

- **The SPI-NOR driver portion is `lpcspifilib` and is imported, not rewritten.** This is everything the manual analysis labelled `spifi_reset_controller`, `spifi_register_family`, `spifi_register_all_families`, `spifi_get_device_size`, `spifi_init_device`, `spifi_detect_device` / `spifi_probe_family` / `spifi_find_device` / `spifi_match_ext_id` / `spifi_read_jedec_id`, `spifi_calc_handle_size`, `list_insert`, `mem_fill`, `spifi_set_memmode` / `spifi_set_options` / `spifi_get_info` / `spifi_in_memory_mode`, `spifi_get_addr_from_block` / `spifi_get_block_from_addr`, `spifi_program` / `spifi_read` / `spifi_erase`, every `spifi_hw_*` accessor, the `spifi_getstatus_*` / `setstatus` / `clearstatus` / `setopts` / `modehook` variants, the `spifi_sel_*` capability selectors, `spifi_status_to_flags`, `spifi_set_block_prot_a` / `_b`, `spifi_cmd_chip_erase`, `spifi_erase_64k_block` / `_4k_sector`, `spifi_page_program`, `spifi_read_data`, and `spifi_build_family_A` / `_B`.

- **The bootloader-specific functions are reconstructed.** The startup/CRT0, the boot logic (`boot_main`, `select_boot_slot`, `image_try_keys`, `is_allowed_entry_addr`, `memzero_words`), the stream cipher (`xorshift128_next`, `prng_seed_*`, `stream_decrypt_*`, `stream_checksum16`), the flash wrapper layer (`mem_read`, `mem_read_any`, `mem_cmp_any`, `mem_is_filled`, `flash_program`, `flash_program_rmw`, `flash_chip_erase`, `buf_all_eq`, `dwords_all_eq`), the small mem helpers (`memcpy_*`, `memset_bytes`, `memcmp_bytes`, `memcpy_fast`), the SPIFI glue wrappers (`spifi_init`, `spifi_deinit`, `spifi_lock_acquire`, `spifi_lock_release`), and the embedded key/config data.

A practical consequence: **the bootloader-specific logic is the entire attack/patch surface.** Everything from `spifi_init` onward is vendor plumbing you can treat as a black box that gives you read/program/erase against `0x14000000`.

### Name mapping — disassembly label → real lpcspifilib API

The reconstructed code calls the real library names (that is how it was actually written); the disassembly labels were manually assigned reverse-engineering aliases. Every call site is annotated `/* IDA: <name> */`. The bootloader only touches these entry points:

| Disassembly label             | Real lpcspifilib API                            | Note                                                           |
| ----------------------------- | ----------------------------------------------- | -------------------------------------------------------------- |
| `spifi_reset_controller`      | `spifiInit(ctrlAddr, reset)`                    | controller bring-up/reset                                      |
| `spifi_register_family`       | `spifiRegisterFamily(regFx)`                    |                                                                |
| `spifi_register_all_families` | `spifi_REG_FAMILY_CommonCommandSet`             | the registration fn passed in                                  |
| `spifi_get_device_size`       | `spifiGetHandleMemSize(ctrlAddr)`               | returns RAM handle/context size, **not** flash size (misnomer) |
| `spifi_init_device`           | `spifiInitDevice(pMem, sz, ctrlAddr, baseAddr)` | builds the handle                                              |
| `spifi_set_options`           | `spifiDevSetOpts(h, opts, set)`                 |                                                                |
| `spifi_set_memmode`           | `spifiDevSetMemMode(h, enMemMode)`              | XIP ↔ command mode                                             |
| `spifi_get_info`              | `spifiDevGetInfo(h, infoId)`                    | id 3 = `SPIFI_INFO_ERASE_BLOCKSIZE`                            |
| `spifi_get_addr_from_block`   | `spifiGetAddrFromBlock(h, blk)`                 |                                                                |
| `spifi_get_block_from_addr`   | `spifiGetBlockFromAddr(h, addr)`                |                                                                |
| `spifi_program`               | `spifiProgram(h, addr, buf, bytes)`             |                                                                |
| `spifi_read`                  | `spifiRead(h, addr, buf, bytes)`                |                                                                |
| `spifi_erase`                 | `spifiErase(h, firstBlk, numBlks)`              |                                                                |

Verify the exact spellings against the lpcspifilib revision you vendor — the API has had minor revisions; these are the stable LPCOpen names from the 2.x line that matches a 2021 build. Two names to double-check: `spifiDevUnlockDevice(h)` (the block-protect unlock that the disassembly shows as the vtable-idx0 call `(*h)[0](h,0,0)`) and the exact spelling of `SPIFI_INFO_ERASE_BLOCKSIZE`.

---

## 7. Manually assigned function labels

The flash dump does not contain function symbols or original function names. The labels below were assigned manually during analysis based on each routine's behavior, call context, and relationship to known LPCOpen/lpcspifilib code. The reconstruction keeps these labels so the source can be compared directly with the disassembly.

| Address      | Function label                |
| ------------ | ----------------------------- |
| `0x14000260` | `IRQ52_Handler`               |
| `0x14000370` | `Reset_Handler`               |
| `0x140003a8` | `jump_to_ram_stage`           |
| `0x140003b8` | `is_allowed_entry_addr`       |
| `0x140003f4` | `image_try_keys`              |
| `0x14000488` | `select_boot_slot`            |
| `0x14000538` | `memzero_words`               |
| `0x1400054c` | `boot_main`                   |
| `0x14000708` | `prng_seed_from_key`          |
| `0x14000728` | `prng_seed_keyA`              |
| `0x14000734` | `prng_seed_keyB_or_device`    |
| `0x14000778` | `xorshift128_next`            |
| `0x1400079c` | `stream_decrypt_skip_header`  |
| `0x14000834` | `stream_checksum16`           |
| `0x140008b8` | `stream_decrypt_segment`      |
| `0x14000974` | `spifi_init`                  |
| `0x140009f8` | `spifi_deinit`                |
| `0x14000a28` | `mem_read`                    |
| `0x14000a34` | `flash_chip_erase`            |
| `0x14000ae4` | `buf_all_eq`                  |
| `0x14000afc` | `flash_program`               |
| `0x14000bc0` | `flash_program_rmw`           |
| `0x14000da8` | `mem_read_any`                |
| `0x14000de4` | `mem_cmp_any`                 |
| `0x14000e50` | `mem_is_filled`               |
| `0x14000eb8` | `dwords_all_eq`               |
| `0x14000ed0` | `memcmp_bytes`                |
| `0x14000eec` | `memcpy_words`                |
| `0x14000f04` | `memcpy_halfwords`            |
| `0x14000f30` | `memcpy_auto`                 |
| `0x14000f4c` | `memset_bytes`                |
| `0x14000f5c` | `spifi_lock_release`          |
| `0x14000f7c` | `spifi_lock_acquire`          |
| `0x14000f9c` | `spifi_hw_write_ctrl`         |
| `0x14000fb8` | `spifi_hw_read_ctrl`          |
| `0x14000fd0` | `spifi_hw_read_stat`          |
| `0x14000fe8` | `spifi_hw_write_cmd`          |
| `0x14001004` | `spifi_hw_read_data_byte`     |
| `0x14001020` | `spifi_hw_write_idata`        |
| `0x1400103c` | `spifi_hw_write_mcmd`         |
| `0x14001058` | `spifi_hw_reset`              |
| `0x14001080` | `spifi_hw_wait_cmd`           |
| `0x140010a4` | `spifi_hw_wait_reset`         |
| `0x140010c8` | `spifi_match_ext_id`          |
| `0x14001134` | `spifi_find_device`           |
| `0x140011ac` | `spifi_read_jedec_id`         |
| `0x14001230` | `spifi_probe_family`          |
| `0x14001304` | `spifi_detect_device`         |
| `0x1400134c` | `spifi_calc_handle_size`      |
| `0x14001368` | `mem_fill`                    |
| `0x140013a8` | `spifi_register_family`       |
| `0x140013ec` | `list_insert`                 |
| `0x1400142c` | `spifi_reset_controller`      |
| `0x14001474` | `spifi_dev_to_cmd_mode`       |
| `0x140014a0` | `spifi_dev_to_mem_mode`       |
| `0x140014cc` | `spifi_get_info`              |
| `0x1400164c` | `spifi_in_memory_mode`        |
| `0x1400167c` | `spifi_set_memmode`           |
| `0x14001740` | `spifi_get_device_size`       |
| `0x14001774` | `spifi_init_device`           |
| `0x140018b0` | `spifi_set_options`           |
| `0x14001998` | `spifi_get_addr_from_block`   |
| `0x140019d8` | `spifi_get_block_from_addr`   |
| `0x14001a18` | `spifi_program`               |
| `0x14001a8c` | `spifi_read`                  |
| `0x14001b00` | `spifi_erase`                 |
| `0x14001b5c` | `spifi_read_stat_reg`         |
| `0x14001b74` | `spifi_write_cmd_reg`         |
| `0x14001b90` | `spifi_write_addr_reg`        |
| `0x14001bac` | `spifi_read_data_byte`        |
| `0x14001bc8` | `spifi_read_data_word`        |
| `0x14001be0` | `spifi_write_data_byte`       |
| `0x14001c00` | `spifi_write_data_word`       |
| `0x14001c1c` | `spifi_hw_write_idata2`       |
| `0x14001c38` | `spifi_wait_cmd_complete`     |
| `0x14001c5c` | `spifi_cmd_write_enable`      |
| `0x14001c7c` | `spifi_wait_device_ready`     |
| `0x14001ca4` | `spifi_status_modify_bit`     |
| `0x14001cfc` | `spifi_device_status_check`   |
| `0x14001d38` | `spifi_getstatus_2sr_norm`    |
| `0x14001dd4` | `spifi_getstatus_3sr`         |
| `0x14001e44` | `spifi_getstatus_1sr`         |
| `0x14001e7c` | `spifi_getstatus_2sr`         |
| `0x14001f00` | `spifi_clearstatus_30h`       |
| `0x14001f2c` | `spifi_write_status_2byte`    |
| `0x14001f80` | `spifi_write_status_3byte`    |
| `0x14001fe0` | `spifi_write_status_1byte`    |
| `0x14002024` | `spifi_setopts_qe_bit6`       |
| `0x14002054` | `spifi_setopts_qe_bit9`       |
| `0x1400209c` | `spifi_modehook_clear_opts`   |
| `0x14002264` | `spifi_sel_modehook`          |
| `0x140022ac` | `spifi_sel_clearstatus`       |
| `0x140022f4` | `spifi_sel_getstatus`         |
| `0x14002368` | `spifi_sel_setstatus`         |
| `0x140023c4` | `spifi_sel_setopts`           |
| `0x1400240c` | `spifi_sel_getreadcmd`        |
| `0x14002454` | `spifi_sel_getprogcmd`        |
| `0x140024b0` | `spifi_status_to_flags`       |
| `0x1400254c` | `spifi_set_block_prot_b`      |
| `0x140025c8` | `spifi_set_block_prot_a`      |
| `0x14002644` | `spifi_cmd_chip_erase`        |
| `0x1400268c` | `spifi_erase_64k_block`       |
| `0x1400274c` | `spifi_erase_4k_sector`       |
| `0x140027ec` | `spifi_page_program`          |
| `0x14002930` | `spifi_read_data`             |
| `0x14002a54` | `spifi_build_family_A`        |
| `0x14002b20` | `spifi_build_family_B`        |
| `0x14002bec` | `spifi_register_all_families` |
| `0x14002e14` | `memcpy_fast`                 |

---

## 8. Build prerequisites

- **IDE:** LPCXpresso or its successor **MCUXpresso IDE**.
- **Toolchain:** GNU Arm Embedded (`arm-none-eabi-gcc`). This is a C project — the "GNU C++" reported in the IDA header reflects only the managed-build linker driver, not the language.
- **NXP LPCOpen for LPC43xx:** the LPCOpen package for an LPC4337/LPC43xx LPCXpresso board (e.g. the `lpcopen_*_lpcxpresso_nxp_lpcxpresso_4337` bundle). You only need the chip library: CMSIS core headers, `chip.h`, and the SCU/RGU/CREG headers. The bootloader links very little of it (it pokes SCU/CREG/RGU/NVIC directly), so a minimal subset compiles fine.
- **NXP lpcspifilib:** the standalone SPIFI library, also shipped inside the LPCOpen examples tree (`.../lpcspifilib/`). It provides `spifilib_api.h`, `spifilib_dev.h`, `spifilib_dev.c`, and the family file `spifilib_fam_standard_cmd_set.c`.

**Version caveat:** the image was built Aug 2021, so match a contemporaneous lpcspifilib revision. The public API names used here are stable across the 2.x/3.x line, but if a name differs in your vendored copy, only the `/* IDA: … */`-tagged call sites in `spifi_glue.c` / `flash_if.c` change.

---

## 9. Building in MCUXpresso

Create three managed-build projects in one workspace, matching the folder tree:

```
80K_4320_Bootloader/         (workspace)
├─ lpc_chip_43xx/   →  Static Library  (LPCOpen chip lib, imported)
├─ lpcspifilib/     →  Static Library  (NXP lpcspifilib, imported)
└─ bootloader/      →  Executable      (this reconstruction; references the two libs)
```

In the **bootloader** project properties:

- **MCU / linker** — select the LPC43xx part; under _C/C++ Build → Settings → Managed Linker Script_, turn the managed script **off** and point _Linker → Linker script_ at `bootloader/ld/lpc43xx_spifi_boot.ld` (the bootloader needs the custom flash-`@0x14000000` / SRAM-relocation layout, not the stock script).
- **Includes** — add `bootloader/inc`, `lpcspifilib/inc`, and the LPCOpen `inc` directories.
- **References** — add `lpc_chip_43xx` and `lpcspifilib` as project references so they're built and linked.
- **Symbols** — define the LPCOpen part macros the chip headers expect (e.g. `CORE_M4`, and the chip family define such as `CHIP_LPC43XX`).

**Toolchain flags:**

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mfpu=fpv4-sp-d16
-std=gnu11 -Os -ffunction-sections -fdata-sections -fno-strict-aliasing
# link:
-nostartfiles -Wl,--gc-sections
```

`-nostartfiles` because we supply `startup_lpc43xx.c`. `-fno-strict-aliasing` because the flash layer type-puns addresses freely. The one mandatory non-default is already baked into `startup_lpc43xx.c`: `Reset_Handler` carries `optimize("no-tree-loop-distribute-patterns")` so its inline copy/zero loops aren't turned into `memcpy`/`memset` calls into not-yet-relocated SRAM (which would fault). See [§12](#12-boot-flow-end-to-end).

---

## 10. Memory map

### Flash (external SPI-NOR, memory-mapped at `0x14000000`)

| Region                       | Address                   | Notes                                  |
| ---------------------------- | ------------------------- | -------------------------------------- |
| Bootloader image (this file) | `0x14000000`–`0x14010000` | 64 KB                                  |
| Per-device key slot          | `0x14000218`              | 16 bytes, blank (`0xFF`) in this dump  |
| Boot-config record           | `0x14010000`              | 16 bytes; first dword selects the slot |
| Firmware slot A              | `0x14030000`              | encrypted application                  |
| Firmware slot B              | `0x14050000`              | encrypted application                  |
| Recovery / golden slot       | `0x14070000`              | encrypted application                  |

Slots are `0x20000` (128 KB) apart. The boot-config base (`0x14010000`) is also handed to the booted application.

### RAM (the layout `Reset_Handler` establishes)

```
0x10000000 ┐ (decrypted app lands here; 0x200/204/208 = handoff info block)
           │
0x10014000 ┤ bootloader code  (RAM copy of flash 0x140003B8+, 0x2B90 bytes)
0x10016B90 ┤ keys/config blob (Key A @..B94, Key B @..BA4; copied from 0x14002F6C)
0x10016FFC ┤ SPIFI BSS (driver scratch + handle + lock refcount) — zeroed
0x10017800 ┤ 0xCDCDCDCD stack guard  ← becomes the app's SP top
0x10018000 ┘ MSP (bootloader stack top)
```

### Flash ↔ RAM relocation formula

`Reset_Handler` relocates flash `0x140003B8 → RAM 0x10014000`. So for any RAM address `R` in the relocated code:

```
flash = R - 0x10014000 + 0x140003B8
```

This is how vtable function pointers in the RAM copy map back to their flash bodies.

---

## 11. The "80K" in the name

The "80K" falls straight out of the linker map. The bootloader's runtime region (`RAM_BOOT`) begins at `0x10014000`, which is exactly `0x10000000 + 80 KB`. The decryptor drops the application into the **low 80 KB** of the SRAM bank (`0x10000000`–`0x10014000`) and the bootloader keeps its own runtime — relocated code, keys, BSS, stack — in the 16 KB above it (`0x10014000`–`0x10018000`).

So `80K_4320_Bootloader` literally encodes _where the loader steps aside to make room for the payload_. (`4320` appears to be an internal product/board designator; its precise meaning is not recoverable from the image alone.)

---

## 12. Boot flow, end to end

Traced against the analyzed dump, so the addresses and values are the real ones.

### Phase 0 — The part's boot ROM hands off

On power-up the LPC43xx internal boot ROM runs first, strapped to boot from SPIFI. It maps the SPI-NOR at `0x14000000` and follows the Cortex-M reset convention:

- `MSP ← *(0x14000000) = 0x10018000`
- `PC  ← *(0x14000004) = 0x14000371` (Thumb), i.e. `Reset_Handler` at `0x14000370`, still executing in place (XIP) from flash.

### Phase 1 — `Reset_Handler` / CRT0 (running from flash)

1. `CPSID i` — mask interrupts.
2. `CREG_M4MEMMAP = 0x10000000` (alias the local SRAM bank to address 0); `VTOR = 0x14000000` (flash vector table for now; every vector except reset is a `WFI` trap).
3. Re-set `MSP = 0x10018000`; write the `0xCDCDCDCD` guard at `0x10017800`.
4. Reset peripherals: `RGU_RESET_CTRL0 = 0x10DF1000`, `RGU_RESET_CTRL1 = 0x01DFF7FF`; clear all NVIC pending (`0xFFFFFFFF` to `ICPR0..7`).
5. Run the scatter-load tables:
   - **Zero BSS:** `0x198` bytes at `0x10016FFC`.
   - **Copy `.data` (the keys):** `0x46C` bytes from flash `0x14002F6C → 0x10016B90`. This is what puts Key A (`0x10016B94`) and Key B (`0x10016BA4`) in RAM.
   - **Copy `.text` (the real logic):** `0x2B90` bytes from flash `0x140003B8 → 0x10014000`.
6. Tail-jump to `jump_to_ram_stage`.

**Why relocate to RAM:** `select_boot_slot` may erase and reprogram the SPIFI flash (the config block), which means leaving memory-mapped mode and driving the controller with explicit commands. You cannot fetch your own instructions from flash while doing that, so the logic must run from SRAM. This is also why the CRT0 copy/zero loops are written **inline** with an optimization barrier — at that point the SRAM copy doesn't exist yet, so the startup must not call `memcpy`/`memset` (which live in `.text_ram`).

### Phase 2 — Into RAM

`jump_to_ram_stage` loads `0x10014195` (Thumb bit set) into `R12` and `BX`es. From here, execution is from SRAM: `0x10014194` is the relocated copy of `boot_main`.

### Phase 3 — `boot_main` (running from RAM)

1. **Pin-mux the SPIFI bus:** `SCU_SFSP3_3..8 = 0xD3, 0xF3, 0xD3, 0xD3, 0xD3, 0x13`.
2. **`spifi_init()`** — reset controller, register the device-family database, probe JEDEC ID (a 4 MB part here), build the handle, apply the quad option, enter memory-mapped mode.
3. **`select_boot_slot(0)`** — read the 16-byte config at `0x14010000`. Selector dword is `0x00000000` (blank) → try slot A first; `image_try_keys(0x14030000)` validates, so it returns `0x14030000`.
4. **`image_try_keys(0x14030000)`** again to learn the key id. Read the cleartext header at `slot+0x200` (`magic 0xA1B2C3D4`, `len 0xAEE8`); try Key B/device first — `stream_checksum16` over `0xAEE8` bytes yields exactly `0x0000FFFF` → pass; decrypt the first 8 bytes, validate SP `0x10017800`; return **1 (Key B)**.
5. **Seed the PRNG with Key B:** state `[26F84503, BA7B858F, 69CA777A, 99A4CC9D]`.
6. **Choose the path.** `magic` matches and `len 0xAEE8 < 0x1C000` → **monolithic path:**
   - Footer address `= slot + ((len>>14)+1)*0x4000 - 0x40 = 0x1403BFC0`. Read 64 bytes; tag is `"CODE"` (`0x45444F43`) and footer length equals `0xAEE8`.
   - `stream_decrypt_skip_header(0x10000000, slot, 0xAEE8, state)` decrypts the whole image to SRAM, leaving words 128–143 verbatim.
   - Hand off: `VTOR = 0x10000000`; `MSP = *(0x10000000) = 0x10017800`; `R0 = slot = 0x14030000`; `BLX *(0x10000004) = 0x10000409`. Control never returns.

### The segmented (fallback) path — not taken here, but part of the machine

If the monolithic checks fail (wrong magic, `len ≥ 0x1C000`, bad `"CODE"` footer, or decrypt error), `boot_main` instead decrypts a `0x2C0`-byte segment table, then for each descriptor fast-forwards the PRNG and `stream_decrypt_segment`s it into its load address, zero-fills each segment's BSS, publishes a handoff block at `0x10000200/204/208`, enables IRQs, and jumps the entry. Note the two paths differ in handoff convention: the monolithic app gets only MSP/entry plus its slot base in `R0`; the segmented app gets the published handoff block and runs with interrupts already enabled.

### Failure behavior

There is no rich error handling. A null slot base or a failed segment decrypt parks the core in a `while(1)` spin. Recovery is purely at the slot level (the A/B/golden ordering). A corrupt image that still checksums correctly but misbehaves is the application's problem, not the loader's.

### One-line summary for this dump

```
ROM      : map SPIFI @0x14000000; MSP=0x10018000; jump 0x14000370
CRT0     : mask IRQ; VTOR=0x14000000; reset peripherals; clear NVIC pending
           zero BSS @0x10016FFC; copy keys ->0x10016B90; copy code ->0x10014000
           jump RAM boot_main @0x10014194
boot_main: pinmux P3; spifi_init -> XIP up (4 MB part)
           select_boot_slot(0): config selector=0 -> slot A 0x14030000
           image_try_keys -> 1 (Key B; checksum 0x0000FFFF; SP 0x10017800 ok)
           seed PRNG with Key B
           header @0x14030200: magic A1B2C3D4, len 0xAEE8 (<0x1C000 -> monolithic)
           footer @0x1403BFC0: "CODE", len 0xAEE8  (ok)
           decrypt 0xAEE8 bytes -> 0x10000000
           VTOR=0x10000000; MSP=0x10017800; R0=0x14030000; BLX 0x10000409
app      : running from SRAM, owns 0x10000000..0x10018000
```

---

## 13. Image format

An application image is a stream of 32-bit words, encrypted with the positional keystream cipher, with two cleartext landmarks:

### Cleartext header window (always plaintext)

Words **128–143** (byte offset **`0x200`–`0x23F`**, 64 bytes) are stored **verbatim, not XORed**. This is the cleartext image header that the loader reads _before_ it knows the key:

- `header[0]` = magic `0xA1B2C3D4`
- `header[1]` = image length in bytes
- (for the segmented format, `header[4]`/word 132 = the app entry pointer)

### "CODE" footer (monolithic only)

A 64-byte plaintext footer sits at the 16 KB-aligned top of the image region:

```
footer_addr = slot + ((len>>14)+1)*0x4000 - 0x40
footer[0]   = 0x45444F43  ("CODE")
footer[1]   = image length (must equal header length)
```

It's a cheap "is this really a complete code image" check, read straight from flash before the loader pays for a full decrypt.

### Monolithic vs. segmented

|                | Monolithic                                   | Segmented (fallback)                                |
| -------------- | -------------------------------------------- | --------------------------------------------------- |
| Selected when  | `magic` ok and `len < 0x1C000` and footer ok | otherwise                                           |
| Decrypt target | one blob to `0x10000000`                     | scattered per-segment load addresses                |
| Table          | none                                         | `0x2C0`-byte segment table                          |
| BSS            | (app handles)                                | zero-filled per descriptor by the loader            |
| Handoff        | MSP + entry, slot base in `R0`               | handoff block at `0x10000200/204/208`, IRQs enabled |
| Entry          | `*(0x10000004)`                              | segment-table word 132                              |

For the segmented table, the observed word offsets (verbatim from the disassembly; surrounding struct semantics inferred) are:

```
[128..143]  cleartext header window; word 132 = app entry pointer
[144..155]  pass-1 load descriptors: 4 × { srcRef, dstVMA, byteLen }
[156..163]  BSS descriptors:         4 × { addr, byteLen }
[164..175]  pass-2 load descriptors: 4 × { srcRef, dstVMA, byteLen }
```

`srcRef` is a bootloader-relative address; the read offset within the slot is `srcRef - 0x14000000`.

---

## 14. The slot system

Three firmware slots plus a 16-byte config record drive selection. `select_boot_slot(update_flag)` reads the record at `0x14010000` and returns the chosen slot base.

### Selection logic (`update_flag == 0`, the normal call)

The selector dword (`cfg[0]`) decides the preference order; each candidate is gated through `image_try_keys`:

- **blank** (`0x00000000` or `0xFFFFFFFF`) → try **A**, then **B**, then **recovery**
- **`1`** → prefer **B**, then **A**, then **recovery**
- **any other value** (e.g. `2`) → prefer **recovery**, then **A**, then **B**

The predicate the binary uses is `(uint32_t)(cfg[0]-1) <= 0xFFFFFFFD`, which is false only for the two blank values. That is the whole "blank → prefer A" branch.

### Rollback path (`update_flag != 0`)

This is the hook a host/app calls to flip the device into recovery: it re-resolves the current slot, and if it's A or B, stamps the flag over that slot's header magic (invalidating it), forces the config selector to `2`, erases and rewrites the config block, and returns recovery. Not reached from `boot_main` in normal operation.

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

### Key → seed transform

The stored 16-byte key (`k0,k1,k2,k3` as little-endian dwords) is whitened with the fixed mask `K = 0x13579BDF` and **rotated by one word** into the PRNG state:

```
x = k1 ^ K
y = k2 ^ K
z = k3 ^ K
w = k0 ^ K          ; note: key word 0 lands in w, not x
```

The rotation is easy to get wrong and changes the entire keystream, so it is worth re-stating: `state = [k1, k2, k3, k0] ^ 0x13579BDF`.

### Decryption — positional XOR

The image is processed one 32-bit word at a time; each ciphertext word is XORed with one keystream word. The PRNG is **positional**: the generator is advanced for _every_ word (so keystream word N lines up with image word N), even for the verbatim header window which is copied unchanged. The segment loader fast-forwards the generator by `image_offset/4` words so the keystream aligns to each segment's position within the overall image.

### Integrity checks

- **Keystream checksum** (`stream_checksum16`): decrypts and sums all plaintext words into a 32-bit accumulator; the image is valid only if the accumulator equals exactly **`0x0000FFFF`**. This is the per-key acceptance test in `image_try_keys`. Note the name says "16" but the comparison is full 32-bit — an image whose low 16 bits happen to be `0xFFFF` still fails unless the high 16 are zero too. (Key B yields `0x0000FFFF`; Key A yields `0x9941791F`.)
- **Entry sanity** (`is_allowed_entry_addr`): the decrypted initial SP must land in valid SRAM (`0x10000000`–`0x10018000`, or `0x10080000`–`0x1008A000`).

### The `image_try_keys` asymmetry

Once the **Key B** checksum passes, the function is **committed to Key B** — if the decrypted SP then fails `is_allowed_entry_addr`, it returns `0` rather than retrying Key A. The Key A path is only reachable when the Key B checksum fails outright. So an image whose checksum matches Key B but whose SP is wrong is rejected entirely, never silently retried under Key A.

---

## 16. Keys and where they live

There are **two embedded 128-bit keys**, plus an optional **per-device override slot**.

### Key A — used by `prng_seed_keyA`

```
RAM (runtime) : 0x10016B94      Flash : 0x14002F70      File offset : 0x2F70
Bytes (16)    : 29 F9 CA CA 38 D0 99 7A 62 58 CF BC 5C 2D 30 4F
xorshift seed : x=0x69CE4BE7  y=0xAF98C3BD  z=0x5C67B683  w=0xD99D62F6
```

### Key B — used by `prng_seed_keyB_or_device` (fallback when the device slot is blank)

```
RAM (runtime) : 0x10016BA4      Flash : 0x14002F80      File offset : 0x2F80
Bytes (16)    : 42 57 F3 8A DC DE AF 35 50 1E 2C A9 A5 EC 9D 7A
xorshift seed : x=0x26F84503  y=0xBA7B858F  z=0x69CA777A  w=0x99A4CC9D
```

### Per-device override slot

```
Flash : 0x14000218 (file offset 0x218)
Bytes : FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF   (BLANK)
```

`prng_seed_keyB_or_device` reads the 16 bytes at `0x14000218`. A slot that is **all-`0x00`** _or_ **all-`0xFF`** is treated as blank → Key B is used. Only a slot that is neither is used as the key. In this dump the slot is erased, so Key B is active.

### How the keys get into RAM

`Reset_Handler` copies `0x46C` bytes from flash `0x14002F6C → 0x10016B90`. The first words of that blob are the boot-config base (`0x14010000`), then Key A, then Key B, then the SPIFI device tables.

> **Decompiler caveat from the original analysis:** Hex-Rays rendered the Key B reference as `dword_10016B94 + 16`, which reads ambiguously, but the literal pool resolves it to `0x10016BA4` — Key B sits **immediately after** Key A (+16 bytes), not +64.

To decrypt a captured image: seed xorshift128 with the matching key's `[x,y,z,w]`, keep words 128–143 verbatim, and XOR every other 32-bit word with one keystream word.

---

## 17. Module reference

### `bootloader.h`

The single shared header: the flash/RAM memory map, image-format magic numbers and offsets, slot bases, return-code legend, the PRNG whitening mask, and prototypes for every reconstructed function. Pointers are real pointers (the Hex-Rays "int-as-address" artifacts are gone); argument order and semantics are preserved exactly; the three gotcha functions carry warning comments.

### `startup_lpc43xx.c`

`g_pfnVectors[]`, `Reset_Handler` (CRT0: vector remap, peripheral reset, NVIC clear, inline BSS-zero / `.data` copy / code relocation), the WFI fault handlers, and the `IRQ52_Handler` catch-all. The single catch-all for all peripheral IRQs is the fingerprint of the stock LPCOpen vector table whose ~49 weak `*_IRQHandler` aliases all resolve to one default; the four `0` entries are NXP's reserved vector slots. **Flash-resident** — executes in place and must not call into not-yet-relocated SRAM, hence the inline loops + `no-tree-loop-distribute-patterns` barrier.

### `boot_main.c`

The logic core: `boot_main` (both paths + handoff), `select_boot_slot`, `image_try_keys`, `is_allowed_entry_addr`, `memzero_words`. Runs from SRAM. The segmented-path word offsets are verbatim from the disassembly.

### `crypto_stream.c`

The keystream cipher: `xorshift128_next`, `prng_seed_from_key` / `prng_seed_keyA` / `prng_seed_keyB_or_device`, `stream_decrypt_skip_header`, `stream_checksum16`, `stream_decrypt_segment`. Verified byte-for-byte against the listing (seed arithmetic, 11/19/8 shifts, verbatim window, 32-bit checksum). `stream_decrypt_skip_header` and `stream_checksum16` are deliberately the same loop differing only by store-vs-accumulate, so the checksum sees exactly the plaintext the decrypt would produce.

### `spifi_glue.c`

Four bootloader wrappers over lpcspifilib: `spifi_init` / `spifi_deinit` (reference-counted around one device handle) and `spifi_lock_acquire` / `spifi_lock_release` (atomic LDREX/STREX refcount that masks IRQs on the 0→1 edge so nothing fetches from XIP while the controller is in command mode). Exposes `g_spifi_handle` for `flash_if.c`.

### `flash_if.c`

The flash-access layer: `mem_read` / `mem_read_any` (address-space-aware copy dispatch), `mem_cmp_any`, `mem_is_filled`, `flash_program` / `flash_program_rmw`, `flash_chip_erase`, `buf_all_eq`, `dwords_all_eq`. Transfers touching the SPIFI window (top byte `0x14`) go through the driver; pure-RAM transfers use the fast helpers. Every library call is `/* IDA: … */`-annotated.

### `util_mem.c`

Small copy/compare/fill primitives mirroring the `__aeabi` copy idioms: `memcpy_words` / `memcpy_halfwords` / `memcpy_auto`, `memset_bytes`, `memcmp_bytes`, and `memcpy_fast` (the toolchain's unrolled `memcpy`, kept by name but deferring to the C library). The odd return values (e.g. address of the last word written) are preserved from the asm; callers ignore them.

### `keys.c`

The embedded key material and boot-config base. Key A, Key B, and the config base live in `.data` (LMA in flash, copied to RAM by CRT0). The per-device slot stays in flash, pinned at `0x14000218` via the `.dev_key` linker section.

### `ld/lpc43xx_spifi_boot.ld`

The custom layout: vectors at `0x14000000`, the `.dev_key` slot fixed at `0x14000218`, the flash-resident `.text_boot`, the LPCXpresso-style scatter-load "Global Section Table," `.text_ram` / `.data` relocated to `0x10014000`+ with LMA in flash, and `.bss` (NOLOAD) ending at `0x10018000`.

---

## 18. Configuration constants

The key tunables (all in `bootloader.h`):

```c
#define SPIFI_XIP_BASE        0x14000000u   /* external SPI-NOR window      */
#define BOOT_CONFIG_BASE      0x14010000u   /* 16-byte boot-config record   */
#define SLOT_A_BASE           0x14030000u
#define SLOT_B_BASE           0x14050000u
#define SLOT_RECOVERY_BASE    0x14070000u
#define SLOT_STRIDE           0x00020000u   /* 128 KB between slots          */
#define DEVICE_KEY_SLOT_ADDR  0x14000218u   /* 16-byte per-unit key override */

#define RAM_APP_LOAD_BASE     0x10000000u   /* decrypted image lands here    */
#define RAM_HANDOFF_INFO      0x10000200u   /* segmented-path info block     */
#define RAM_CODE_BASE         0x10014000u   /* RAM copy of boot code         */
#define MSP_TOP               0x10018000u
#define STACK_GUARD_ADDR      0x10017800u
#define STACK_GUARD_VALUE     0xCDCDCDCDu

#define IMG_HEADER_OFFSET     0x200u        /* cleartext header @ slot+0x200 */
#define IMG_MAGIC             0xA1B2C3D4u
#define IMG_FOOTER_TAG        0x45444F43u   /* "CODE"                        */
#define IMG_MONOLITHIC_MAX    0x1C000u      /* monolithic path if len < this */
#define IMG_MAX_LEN           0x10000u      /* image_try_keys length cap     */
#define SEG_TABLE_BYTES       0x2C0u
#define IMG_HDR_WORD_FIRST    128u          /* 0x200 / 4 (verbatim window)   */
#define IMG_HDR_WORD_LAST     143u
#define SEG_SKIP_WORDS        0x80u
#define PRNG_WHITEN_MASK      0x13579BDFu

#define KEYID_NONE            0
#define KEYID_B_OR_DEVICE     1
#define KEYID_A               2

#define FL_OK                 0
#define FL_VERIFY_FAIL        6
#define FL_BADARG             11            /* 0xB */
#define FL_NOTINIT            2
```

---

## 19. Naming conventions and known misnomers

The manually assigned disassembly labels are preserved verbatim so source and listing line up 1:1. Several labels are **misleading** and are flagged in the code:

| Function                | Name suggests           | Actually does                                                                                                                  |
| ----------------------- | ----------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| `flash_chip_erase`      | erases the whole device | erases the **single 64 KB block** containing `addr`, then verifies it reads all-`0xFF`                                         |
| `mem_is_filled`         | returns true if filled  | returns **1 if the region is NOT uniformly `fill_byte`**, else 0 (a "differs from fill" / blank check)                         |
| `dwords_all_eq`         | nonzero if all equal    | returns **0 if every dword equals `val`**, otherwise the nonzero first-mismatch difference (opposite polarity to `buf_all_eq`) |
| `stream_checksum16`     | 16-bit / mod-2¹⁶ sum    | compares the **full 32-bit** accumulator against `0x0000FFFF` (stricter than a truncated sum)                                  |
| `spifi_get_device_size` | a flash size            | the **RAM handle/context size** (`descriptor.ctxBytes + 0x40`); the boot checks it is ≤ `0x53`                                 |

---

## 20. Reconstruction caveats (load-bearing assumptions)

These assumptions are load-bearing — losing them will break a build or mislead an analyst:

- **`spifiDevUnlockDevice` name** — in the disassembly this is the direct vtable-idx0 call `(*handle)[0](handle, 0, 0)` (block-protect set/clear, arg 0 = unlock). Verify the exact public-API spelling against your vendored lpcspifilib revision; depending on version it may be a block-level unlock or a direct family-fx call, but the semantics (clear the BP bits before erase/program) are fixed.
- **`g_rmw_buf` indirection (`flash_if.c`)** — the image fetches the 64 KB read-modify-write staging-buffer base from a **pointer cell at `0x20000000`** (AHB SRAM). Whatever initializes that cell is **outside** this 64 KB image (board/SDK bring-up). The reconstruction mirrors the indirection rather than inventing a buffer; if you build a runnable bootloader you must populate it.
- **Segmented-path table offsets (`boot_main.c`)** — the word offsets `[132]` (entry), `[144..]` / `[156..]` / `[164..]` (load/BSS/load descriptors) are taken verbatim from the disassembly; the surrounding struct semantics are **inferred**. This image boots the monolithic path, so the segmented path is reconstructed but **unexercised**.
- **CRT0 inline loops** — `Reset_Handler` must keep its copy/zero loops inline (compiled with `no-tree-loop-distribute-patterns`) so GCC does not rewrite them into `memcpy`/`memset` calls that would resolve into not-yet-relocated SRAM.

---

## 21. Intentional oddities preserved from the binary

Two behaviors look like bugs but are faithfully reproduced because they are in the original machine code, not transcription slips:

- **`flash_program_rmw`'s final verify passes length `0`** — `mem_cmp_any(a1, a2, 0)` is a no-op, so that read-back verification effectively does nothing.
- **`flash_chip_erase`'s command-mode-failure path skips `spifi_deinit`** — a small reference-count leak on that error branch. The `goto` layout reproduces it exactly.

---

## 22. Verifying the reconstruction

You do **not** need silicon to sanity-check the logic. The crypto path (`crypto_stream.c` + the `prng_seed_*`) is self-contained and host-compilable. Feed it slot A from your dump (`0x14030000`, length `0xAEE8`), seed with Key B, and you should reproduce exactly what the analysis already confirmed end to end:

```
stream_checksum16(slot, 0xAEE8, seed(KeyB))  == 0x0000FFFF   (Key A would give 0x9941791F)
decrypted MSP   = *(0x10000000)              == 0x10017800
decrypted entry = *(0x10000004)              == 0x10000409
plaintext footer at slot+0xBFC0 (0x1403BFC0): "CODE", length 0xAEE8
```

If those four match, the cipher reconstruction is **bit-exact**; the SPIFI half is then just the imported library doing read/program/erase against `0x14000000`.

## 23. Security analysis

- **Software xorshift, not hardware AES.** Although LPC43**S** parts include an AES engine + OTP, this bootloader protects the application with a software xorshift128 keystream. There is no authentication — no signature or MAC — only a 16-bit additive checksum and an SP-range check.
- **Keys are in the clear in flash.** Both 128-bit keys are constants in the bootloader image; anyone with the dump has the keys.
- **Known-plaintext is fatal.** xorshift128 is fully invertible: ~16 bytes of known plaintext at a known offset recovers part of the state, and the cleartext 64-byte header window (offset `0x200`) plus the standard Cortex-M vector-table structure provide predictable plaintext to attack.
- **The per-device key slot is unused here.** On a provisioned device it could hold a unit-unique key; in this dump it is erased, so the shared Key B is active.
- **The checksum is trivially forgeable.** A 16-bit additive sum imposes no meaningful integrity barrier against an attacker who controls the plaintext.

**Bottom line:** this scheme is symmetric obfuscation. It raises the bar against casual cloning of an encrypted image, and nothing more. Do not adopt it for a threat model that includes a motivated attacker with the bootloader image.

---

## 24. Known issues / TODO

- **Runnability:** populate the `0x20000000` RMW staging-buffer pointer (see [§20](#20-reconstruction-caveats-load-bearing-assumptions)) before expecting program/erase to work on hardware.
- **lpcspifilib version pinning:** lock the vendored revision and confirm the two name-sensitive calls (`spifiDevUnlockDevice`, `SPIFI_INFO_ERASE_BLOCKSIZE`).
- **Segmented path:** unexercised by this image; the inferred struct layout deserves validation against a segmented sample if one is ever captured.
- **Device descriptor blobs:** the lpcspifilib device-table entries in `.data` (the Winbond/Spansion/Macronix descriptors) are not yet annotated symbolically.
- **Status-opcode arrays:** the three status-read opcode arrays at `0x10016CB0` / `CB4` / `CB8` could be marked as byte arrays and named.

Potential follow-up artifacts:

- A companion **host-side encryptor** to round-trip images for either slot/key.
- A standalone analysis script that names the device-descriptor blobs so the lpcspifilib device table reads symbolically.

---

## 25. Glossary

- **SPIFI** — NXP's SPI Flash Interface peripheral; supports memory-mapped (XIP) reads from external SPI-NOR.
- **XIP (eXecute In Place)** — running/reading code directly from memory-mapped flash without copying to RAM.
- **CRT0** — the C runtime startup that initializes `.data`/`.bss` and hands control to the program; here, also the relocation-to-RAM stage.
- **VTOR** — Vector Table Offset Register (`0xE000ED08`); points the core at the active exception vector table.
- **WREN / RDID / RDSR / WRSR** — standard SPI-NOR commands: write-enable (`0x06`), read JEDEC ID (`0x9F`), read/write status register (`0x05`/`0x01`).
- **BP bits** — block-protection bits in the SPI-NOR status register; cleared before erase/program.
- **Monolithic / segmented** — the two image shapes the loader can decrypt (one blob vs. a scatter-loaded segment table).
- **Verbatim window** — the 16-word (`0x200`–`0x23F`) cleartext header region the cipher copies unchanged.

---

## 26. References

- **NXP LPC43xx User Manual (UM10503)** — SPIFI, CREG (`M4MEMMAP`), RGU, SCU, NVIC register details.
- **ARMv7-M Architecture Reference Manual** — Cortex-M4 reset behavior, vector table, VTOR, `MSP`.
- **NXP LPCOpen** — chip layer (CMSIS + peripheral drivers) for LPC43xx.
- **NXP lpcspifilib** — the "Common SPIFI Command Set" SPI-NOR driver imported here; see `spifilib_api.h` / `spifilib_dev.h`.
- **Marsaglia, G., "Xorshift RNGs," Journal of Statistical Software (2003)** — the PRNG family used as the keystream generator (shift triple 11/19/8).

---

_This README documents a reverse-engineering reconstruction. The disassembly is the source of truth; where this document and the listing disagree, trust the listing. The embedded keys are recovered constants, not secrets._
