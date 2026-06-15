/* keys.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Embedded key material, the boot-config base pointer, and the build-info block.
 *
 * This image carries TWO keys plus a dual-purpose mask block:
 *   - g_key_mask : 16 bytes that serve BOTH as Key A (the transport/OTA key,
 *                  seeded directly into the xorshift state) AND as the whitening
 *                  mask XORed into the silicon device-ID when Key B is derived
 *                  from hardware.
 *   - g_keyB     : the fixed embedded Key B (the at-rest device key), used when
 *                  the build marker selects the fixed-key path (it does here).
 * The config base pointer and both keys live in the tail of .data (LMA in flash,
 * copied to RAM by CRT0). The first ~0x13C bytes of .data belong to the imported
 * driver's mutable tables and are part of the driver blob, not this file.
 *
 * NOTE: these are recovered constants, not secrets — they are embedded in clear
 * in the flash image. A 128-bit-keyed xorshift128 plus a 32-bit additive
 * checksum is obfuscation, not authenticated encryption.
 *
 * Build identity: marker {01,02,00,00} (composite 0x01020000), built
 * "Jun 26 2016" "11:08:15". There is NO internal build-name string in this image
 * (the iOS image embedded "80K_43X0_Bootloader"); the workspace name is
 * descriptive.
 */
#include "bootloader.h"

/* .data: boot-config base, later handed to the app via the handoff block.
 * (RAM 0x10011EAC <- flash 0x14002978; value 0x14010000.) */
uint32_t g_boot_config_ptr = BOOT_CONFIG_BASE;     /* 0x14010000 */

/* .data: Key A == the mask block. Seeded directly into the xorshift state for
 * the transport/OTA form, and XORed with the silicon device-ID for Key-B
 * derivation. (RAM 0x10011EB0 <- flash 0x14002ABC, .data+0x140.) */
uint8_t g_key_mask[16] = {
    0x67, 0xA3, 0xEA, 0x21, 0x82, 0x4F, 0xEC, 0xC4,
    0xB3, 0xC3, 0xB0, 0xA8, 0xDA, 0x51, 0x46, 0x69
};

/* .data: fixed Key B (the at-rest device key). Used when the build marker
 * (0x01020000) exceeds the fixed-key threshold (0x01010000), which it does in
 * this image, so this key — not a silicon-derived one — is active.
 * (RAM 0x10011EC0 <- flash 0x14002ACC, .data+0x150.) */
uint8_t g_keyB[16] = {
    0xFA, 0xAC, 0xB3, 0xC6, 0xF1, 0x41, 0x24, 0x69,
    0xBD, 0x12, 0x2F, 0xB8, 0x2D, 0x78, 0x16, 0x0D
};

/* .rodata_flash: build-info block, flash-resident at 0x14002958 (NOT copied to
 * RAM). Its address is published to the app as the info pointer at handoff, and
 * its first 4 bytes are the marker that prng_seed_from_key reads to choose the
 * fixed-vs-silicon Key-B path. Layout mirrors the compiler's __DATE__/__TIME__
 * block: marker, then the date string, then the time string. */
const uint8_t g_build_info[0x24]
    __attribute__((section(".rodata_flash"), used)) = {
    /* +0x00 */ 0x01, 0x02, 0x00, 0x00,
    /* +0x04 */ 'J','u','n',' ','2','6',' ','2','0','1','6', 0, 0, 0, 0, 0,
    /* +0x14 */ '1','1',':','0','8',':','1','5', 0, 0, 0, 0, 0, 0, 0, 0
};