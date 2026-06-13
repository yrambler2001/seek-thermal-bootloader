/* keys.c — 80K_4320_Bootloader (reconstructed)
 *
 * Embedded key material + boot-config base. The boot-config base and the two
 * 128-bit keys live in .data (LMA in flash @0x14002F6C; CRT0 copies them to RAM
 * @0x10016B90, so prng_seed_* read the RAM copies). The optional per-unit key
 * slot stays in flash and is read directly at its fixed address (0x14000218).
 *
 * NOTE: these are obfuscation keys baked into the image — recovered constants,
 * not secrets. xorshift128 + a 16-bit checksum is not authenticated encryption.
 */

#include "bootloader.h"

/* .data: boot-config base, later handed to the app
 * (IDA: g_boot_config_base_flash_word -> RAM g_boot_config_base_ram). */
uint32_t g_boot_config_base = 0x14010000u;

/* .data: embedded Key A   (IDA: g_keyA_flash -> RAM g_keyA @0x10016B94). */
uint8_t g_keyA[16] = {
    0x29, 0xF9, 0xCA, 0xCA, 0x38, 0xD0, 0x99, 0x7A,
    0x62, 0x58, 0xCF, 0xBC, 0x5C, 0x2D, 0x30, 0x4F
};

/* .data: embedded Key B fallback (IDA: g_keyB_flash -> RAM g_keyB @0x10016BA4). */
uint8_t g_keyB[16] = {
    0x42, 0x57, 0xF3, 0x8A, 0xDC, 0xDE, 0xAF, 0x35,
    0x50, 0x1E, 0x2C, 0xA9, 0xA5, 0xEC, 0x9D, 0x7A
};

/* Flash @0x14000218: optional per-unit key override; all-0xFF (erased) in this
 * image. Pinned by the linker via the .dev_key section; prng_seed_keyB_or_device
 * reads it through DEVICE_KEY_SLOT_ADDR. A slot that is all-00 or all-FF is
 * treated as blank -> Key B is used. */
const uint8_t g_device_key_slot_flash[16]
    __attribute__((section(".dev_key"), used)) = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};