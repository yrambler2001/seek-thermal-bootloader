# Seek Thermal Bootloader Reconstructions

**A repository of reverse-engineered, behaviorally faithful C reconstructions of the first-stage secure bootloaders for Seek Thermal cameras (LPC43xx / ARM Cortex-M4).**

This repository contains four complete from-scratch C reconstructions of Seek Thermal boot images recovered by disassembling 4 MB SPI-NOR flash dumps. It also includes a host-side decryption tool capable of automatically locating keys and decrypting firmware payloads from raw flash dumps across the different bootloader generations.

---

## 1. Overview

The bootloaders in this repository live in external SPIFI flash (mapped at `0x14000000`). Their primary job is to initialize the core and SPI-NOR flash controller, select a firmware "slot" based on a boot configuration record, verify and decrypt the application into internal SRAM, and jump to it.

The reconstructions are highly faithful to the original machine code, intended to be byte-for-byte accurate on the cryptographic paths and structurally accurate elsewhere.

### Security Notice

> **The encryption keys in this repository are recovered constants, not secrets.** > Both 128-bit keys are embedded in cleartext in the original flash images. The protection scheme relies on a software **xorshift128 stream cipher**, which provides symmetric obfuscation rather than authenticated encryption. There is no signature or MAC—only a 32-bit additive checksum. A known-plaintext attack trivially recovers the keystream. **Do not reuse this design for anything that requires tamper resistance.**

---

## 2. Repository Structure

- **`Bootloader 2016.06.26 11.08.15 Compact Pro/`**
  The earliest reconstructed bootloader, built for the Compact Pro (Android). It executes its entire boot pipeline in-place (XIP) from flash, utilizes a bespoke hand-written SPIFI driver (imported as a blob rather than `lpcspifilib`), and features a unique two-key (transport vs. at-rest) migration system with no key whitening.
- **`Bootloader 2018.07.05 17.35.20 Compact/`**
  The bootloader for the Seek Thermal Compact Android CW. It drives the SPIFI controller purely in command mode (no XIP), uses an all-trap vector table with no live interrupts, and reserves a 112 KB application region.
- **`Bootloader 2019.01.07 15.56.20 Compact Pro FF/`**
  The bootloader for the Seek Thermal Compact Pro FF. It maps the flash for Execute-In-Place (XIP), installs 3 live RAM-resident peripheral interrupts, and reserves a 112 KB application region.
- **`Bootloader 2021.08.12 08.20.23 Nano 300/`**
  The bootloader for the Seek Thermal Nano 300. It uses XIP memory mapping, an all-trap vector table, reserves an 80 KB application region, and adds an initial Stack Pointer (SP) range check on top of the checksum validation.
- **`decrypt_firmware.js`**
  A standalone Node.js script that scans a full SPI-NOR flash dump, automatically discovers the embedded obfuscation keys (regardless of offset), and extracts/decrypts all valid firmware application images into standard binaries.

---

## 3. Bootloader Comparison

All four bootloaders share the same core architecture (NXP LPC43xx, ARMv7-M, external SPI-NOR) and cryptographic primitives, but feature distinct memory layouts, flash access paradigms, and verification logic.

| Feature              | Compact Pro (2016)         | Compact Android CW         | Compact Pro FF             | Nano 300                   |
| :------------------- | :------------------------- | :------------------------- | :------------------------- | :------------------------- |
| **Build Timestamp**  | Jun 26, 2016               | Jul 5, 2018                | Jan 7, 2019                | Aug 12, 2021               |
| **Flash I/O Mode**   | Memory-mapped (XIP)        | Command Mode (No XIP)      | Memory-mapped (XIP)        | Memory-mapped (XIP)        |
| **SPI-NOR Driver**   | Bespoke Blob               | `lpcspifilib`              | `lpcspifilib`              | `lpcspifilib`              |
| **Interrupts**       | All-trap vector table      | All-trap vector table      | 3 live RAM-resident IRQs   | All-trap vector table      |
| **App Region Size**  | 64 KB                      | 112 KB                     | 112 KB                     | 80 KB                      |
| **SRAM Base**        | `0x10010000`               | `0x1001C000`               | `0x1001C000`               | `0x10014000`               |
| **Initial SP (MSP)** | `0x10018000`               | `0x10020000`               | `0x10020000`               | `0x10018000`               |
| **Key Whitening**    | None (`0x00000000`)        | `0x13579BDF`               | `0x13579BDF`               | `0x13579BDF`               |
| **Acceptance Test**  | 32-bit sum `== 0x00000000` | 32-bit sum `== 0x0000FFFF` | 32-bit sum `== 0x0000FFFF` | Checksum + SP bounds check |

_Note: The "80K" present in the internal build names of several newer bootloaders is a legacy lineage tag; actual application region limits vary between 64 KB, 80 KB, and 112 KB depending on the generation._

---

## 4. Cryptographic Scheme

All four bootloaders protect application firmware using a software **xorshift128 (Marsaglia)** keystream cipher.

- **Initialization:** The 16-byte key is either fed directly into the PRNG state (2016 Compact Pro) or whitened with a constant `0x13579BDF` first (all later variants), then rotated into the PRNG state.
- **Decryption:** Operates on 32-bit words. The keystream advances for every word positionally.
- **Verbatim Window:** Words 128–143 (the 64-byte cleartext header at offset `0x200`) are passed through unencrypted so the loader can inspect the magic number and length prior to decryption.
- **Key Migration (2016 Only):** The 2016 Compact Pro build features a dual-key re-encryption sweep at boot, which actively converts OTA/transport images (Key A) into device-storage images (Key B).
- **Formats:** The newer bootloaders support a **Monolithic** image path (straight decrypt to SRAM) and a **Segmented** fallback path (decrypts a table of scatter-load descriptors and BSS zero-fills). The 2016 build only utilizes the segmented path.

---

## 5. Firmware Decryption Tool

The included `decrypt_firmware.js` allows you to extract payloads directly from an LPC43xx SPI-NOR flash dump. It works across all four bootloader generations by attempting different cipher profiles.

### Prerequisites

- Node.js installed on your host machine.

### Usage

```bash
node decrypt_firmware.js <full_dump.bin> [output_directory]

```

**How it works:**

1. Checks against all known cipher profiles (handling both unwhitened `K=0x0` and whitened `K=0x13579BDF` algorithms).
2. Pre-filters candidate key windows by validating that the resulting decrypted initial SP and reset vector are well-formed.
3. Checks the 32-bit accumulator target (`0x00000000` or `0x0000FFFF`) against the respective profile.
4. Automatically writes decrypted `slot_*.bin` payloads and a detailed decryption report (`.txt`) for every valid firmware image it finds.

---

## 6. Build Instructions (Reconstructions)

To build any bootloader reconstruction, you will need **MCUXpresso IDE** (or LPCXpresso) and the **GNU Arm Embedded Toolchain**.

1. Create a workspace encompassing the specific bootloader folder, the imported `lpc_chip_43xx` LPCOpen library, and either the bespoke driver blob (2016 variant) or the `lpcspifilib` (2018+ variants).
2. Link the project using the custom scatter-load script provided in `ld/lpc43xx_spifi_boot.ld`.
3. Use the `-fno-strict-aliasing`, `-Os`, and `-nostartfiles` flags to ensure the inline `Reset_Handler` routines compile precisely without jumping into non-relocated SRAM.

_For detailed compilation notes, memory mappings, and specific reverse-engineering caveats, consult the dedicated `readme.md` inside each bootloader's respective folder._
