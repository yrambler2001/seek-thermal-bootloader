// CompactPro_43X0_Bootloader/bootloader/inc/bootloader.h
/* bootloader.h — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Names are kept identical to the disassembly. Where a name does not match
 * its behaviour, the prototype carries a comment.
 *
 * Unlike the sibling LPC43xx images, this build embeds NO internal name
 * string. The only identity baked in is a 4-byte build marker + an ASCII
 * build date/time at byte_14002958: marker {01,02,00,00} (composite
 * 0x01020000), "Jun 26 2016" / "11:08:15". The workspace name here is
 * therefore descriptive, anchored to product code UQ-AAA and that date.
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===================================================================== */
/* SPIFI / flash memory map (everything is memory-mapped XIP @0x14000000) */
/* ===================================================================== */
#define SPIFI_XIP_BASE        0x14000000u   /* external SPI-NOR window        */
#define BOOTLOADER_BASE       0x14000000u   /* this image                     */
#define BOOT_CONFIG_BASE      0x14010000u   /* 16-byte boot-config record     */

#define SLOT_A_BASE           0x14050000u   /* firmware slot A                */
#define SLOT_B_BASE           0x14060000u   /* firmware slot B                */
#define SLOT_RECOVERY_BASE    0x14070000u   /* golden / recovery slot         */
#define SLOT_STRIDE           0x00010000u   /* 64 KB between slots            */

/* The SPIFI controller register block (driven directly by the imported
 * bespoke driver; the boot logic never touches it). */
#define SPIFI_CTRL_BASE       0x40003000u

/* ===================================================================== */
/* RAM layout that Reset_Handler establishes                              */
/* ===================================================================== */
#define RAM_APP_LOAD_BASE     0x10000000u   /* decrypted app lands here (64 KB)*/
#define RAM_HANDOFF_BLOCK     0x10000200u   /* bootloader -> app handoff cells */
/*   +0x00 = build-info pointer (&byte_14002958)                            */
/*   +0x04 = boot-config pointer (*dword_10011EAC == 0x14010000)            */
/*   +0x08 = slot indicator: 0 = A, 1 = B, 2 = recovery                     */
#define RAM_UPDATE_FLAG       0x1000020Cu   /* app->bootloader update request  */
#define RAM_UPDATE_MAGIC      0x10000210u   /* 64-bit gate paired with it      */
/* Only the bespoke SPIFI driver is relocated to RAM (it is what drops out
 * of XIP); the boot logic itself runs in place from flash. */
#define RAM_DRIVER_BASE       0x10010000u   /* relocated SPIFI driver (0x1D70) */
#define RAM_REQUEST_STRUCT    0x10011ED0u   /* marshalled op request (.bss)    */
#define RAM_DRIVER_CONTEXT    0x10011EE4u   /* driver handle/context (.bss)    */
#define MSP_TOP               0x10018000u   /* bootloader initial MSP          */
/* Read/modify/write staging buffer base, in AHB SRAM (separate bank). */
#define STAGING_BUF_BASE      0x20000000u

/* ===================================================================== */
/* Image format                                                           */
/* ===================================================================== */
#define IMG_HEADER_OFFSET     0x200u        /* cleartext header @ slot+0x200   */
#define IMG_MAGIC             0xA1B2C3D4u    /* header word[0]                  */
#define IMG_MAX_LEN           0x10000u       /* image length cap                */
#define SEG_TABLE_BYTES       0x2C0u         /* segment-table size              */

/* The cipher copies words 128..143 (bytes 0x200..0x23F) verbatim.          */
#define IMG_HDR_WORD_FIRST    128u           /* 0x200 / 4                       */
#define IMG_HDR_WORD_LAST     143u           /* inclusive                       */
#define SEG_SKIP_WORDS        0x80u          /* skip_words for segments (= 128) */

/* NOTE: there is NO monolithic path and NO "CODE" footer in this build —
 * every accepted image is loaded through the segmented path below. There is
 * also NO key-whitening constant: key words seed the PRNG directly. */
/* Acceptance sentinel: an image is valid iff its decrypted word-sum is 0. */
#define ACCEPT_SENTINEL       0x00000000u
/* Build-marker gate read by prng_seed_from_key: composite of byte_14002958. */
#define BUILD_MARKER          0x01020000u    /* {01,02,00,00}                  */
#define BUILD_MARKER_FIXEDKEY 0x01010000u    /* marker > this -> fixed Key B   */
/* Silicon registers Key B is derived from when the marker selects that path. */
#define SILICON_ID_BASE       0x40045000u    /* 4 dwords XORed with the mask   */
/* Warm-boot update mailbox sentinels. The flag is honoured only when it is
 * one of these two values, i.e. (flag + 0x55AA00FF) <= 1. */
#define UPDATE_FLAG_A         0xAA55FF01u
#define UPDATE_FLAG_B         0xAA55FF02u
#define UPDATE_MAGIC_GATE     0x752Fu        /* qword must be <= this to honour */
/* slot indicator values published at RAM_HANDOFF_BLOCK+8 */
#define SLOTID_A              0
#define SLOTID_B              1
#define SLOTID_RECOVERY       2
/* SPIFI request-struct opcodes / flags (see flash_if.c) */
#define SPIFI_OP_PROGRAM      0x08u
#define SPIFI_OP_ERASE        0x20u
#define SPIFI_STAGE_FLAG      0x20000000u

/* Flash-layer status codes (as returned by the disassembly) */
#define FL_OK                 0
#define FL_BADARG             11             /* 0xB: out of range / misalign   */
#define FL_NOTINIT            2
/* ===================================================================== */
/* Prototypes — names verbatim from the disassembly (one rename, noted)   */
/* ===================================================================== */

/* --- startup_lpc43xx.c --- */
void      Reset_Handler(void) __attribute__((noreturn));
void     *scatterload_copy_words(void *dst, const void *src, uint32_t byte_len);

/* --- boot_main.c --- */
void      memzero_words(void *dst, uint32_t byte_len);
uint32_t  select_boot_slot(int update_flag);          /* returns slot base addr */
/* image_try_keys / image_try_keys_copy2: each validates a slot under ONE key
 * (magic + len + checksum). _copy uses Key A; _copy2 uses Key B. */
int       image_try_keys(const uint32_t *slot_base);        /* Key A */
int       image_try_keys_copy2(const uint32_t *slot_base);  /* Key B */
/* image_checksum_ok[_copy2]: return 1 iff the decrypted word-sum == 0. */
int       image_checksum_ok(const uint32_t *src, uint32_t byte_len);       /* A */
int       image_checksum_ok_copy2(const uint32_t *src, uint32_t byte_len); /* B */

/* --- crypto_stream.c --- */
/* prng_seed_from_key: seeds the Key-B / silicon-derived state (no whitening).
 * The Key-A seed is inlined at its call sites; there is no separate helper,
 * and no standalone xorshift128_next (the PRNG step is inlined everywhere). */
uint32_t *prng_seed_from_key(uint32_t *state);
/* stream_checksum16[_copy2]: sum plaintext words; return BOTH Σraw and Σdec
 * via out-params (Σraw is vestigial — computed, never read). _copy keys from
 * the mask block directly (Key A); _copy2 keys via prng_seed_from_key (Key B).
 * The "16" is a carryover; the accept test is the full 32-bit Σdec == 0. */
void      stream_checksum16(const uint32_t *src, uint32_t byte_len,
                            uint32_t *sum_raw, uint32_t *sum_dec);
void      stream_checksum16_copy2(const uint32_t *src, uint32_t byte_len,
                                  uint32_t *sum_raw, uint32_t *sum_dec);
/* RENAMED from the listing's stream_decrypt_skip_header (0x1400093c). Runs two
 * keystreams in lockstep (ksA from the mask block, ksB from prng_seed_from_key)
 * and XORs BOTH into each body word, leaving words 128..143 verbatim. Converts
 * a Key-A "transport" slot to its Key-B "at-rest" form. */
int       stream_reencrypt_keyA_to_keyB(void *dst, const uint32_t *src,
                                        uint32_t byte_len);
/* The genuine single-key (Key B) skip-header decryptor: entry sets up, body
 * does the work. Used to decrypt the 0x2C0-byte segment table. */
int       stream_decrypt_skip_header_entry(void *dst, const uint32_t *src,
                                           uint32_t byte_len);
int       stream_decrypt_skip_header_body(void *dst, const uint32_t *src,
                                          uint32_t byte_len, uint32_t *state);
/* Positional segment decrypt (Key B): reseed, fast-forward by image_offset/4
 * words, skip the 16-word window at skip_words. */
int       stream_decrypt_segment(void *dst, uint32_t image_base,
                                 uint32_t image_offset, uint32_t byte_len,
                                 uint32_t skip_words);

/* --- flash_if.c --- */
int       spifi_init(void);          /* pin-mux + fill request + driver init    */
int       flash_program(uint32_t flash_off, const void *src, uint32_t len,
                        int staged);
int       flash_erase_region(uint32_t flash_off, uint32_t len);
int       flash_program_rmw(uint32_t flash_off, const void *src, uint32_t len);
/* flash_chip_erase: despite the name, erases the single 64 KB block at
 * flash_off (fixed size 0x10000); used by the A->B migration loop. */
int       flash_chip_erase(uint32_t flash_off);
void     *memcpy_auto(void *dst, const void *src, int len);   /* word + byte tail */
void     *memcpy_bytes(void *dst, const void *src, int len);  /* byte-only        */

/* --- spifi_glue.c (the imported bespoke SPIFI driver: interface only) --- */
/* The three flash-resident thunks into the relocated RAM driver. */
int       spifi_drv_init_thunk(void *ctx, const void *src, void *req);    /* cfg  */
int       spifi_drv_program_thunk(void *ctx, const void *src, void *req); /* prog */
int       spifi_drv_op_thunk(void *ctx, const void *src, void *req);      /* erase*/

/* --- keys.c (placed at fixed RAM addresses by the linker; see ld script) --- */
extern const uint8_t  g_key_mask[16];   /* unk_10011EB0: Key A AND silicon mask */
extern const uint8_t  g_keyA[16];       /* unk_10011EC0-style: validation Key A */
extern const uint8_t  g_keyB[16];       /* fixed Key B (storage)                */
extern const uint32_t g_boot_config_ptr;/* dword_10011EAC == 0x14010000         */
extern const uint8_t  g_build_info[];   /* byte_14002958: marker + date/time    */

#endif /* BOOTLOADER_H */