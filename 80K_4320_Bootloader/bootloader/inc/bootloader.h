/* bootloader.h — 80K_4320_Bootloader (reconstructed)
 *
 * Names are kept identical to the disassembly. Where a name does not match
 * its behaviour, the prototype carries a comment.
 *
 * Build identity baked into the image: g_internal_build_name =
 * "80K_4320_Bootloader", built "Aug 12 2021 08:20:23".
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===================================================================== */
/* SPIFI / flash memory map (everything is memory-mapped XIP @0x14000000) */
/* ===================================================================== */
#define SPIFI_XIP_BASE        0x14000000u   /* external SPI-NOR window      */
#define BOOTLOADER_BASE       0x14000000u   /* this image (64 KB)           */
#define BOOT_CONFIG_BASE      0x14010000u   /* 16-byte boot-config record   */

#define SLOT_A_BASE           0x14030000u   /* firmware slot A              */
#define SLOT_B_BASE           0x14050000u   /* firmware slot B              */
#define SLOT_RECOVERY_BASE    0x14070000u   /* golden / recovery slot       */
#define SLOT_STRIDE           0x00020000u   /* 128 KB between slots          */

/* Optional per-unit key override; all-0xFF (blank) in this dump.          */
#define DEVICE_KEY_SLOT_ADDR  0x14000218u   /* 16 bytes, read from flash     */

/* ===================================================================== */
/* RAM layout that Reset_Handler establishes                              */
/* ===================================================================== */
#define RAM_APP_LOAD_BASE     0x10000000u   /* decrypted image lands here    */
#define RAM_HANDOFF_INFO      0x10000200u   /* segmented-path info block     */
#define RAM_CODE_BASE         0x10014000u   /* RAM copy of boot code         */
#define RAM_DATA_BASE         0x10016B90u   /* RAM copy of keys/config       */
#define MSP_TOP               0x10018000u   /* bootloader initial MSP        */
#define STACK_GUARD_ADDR      0x10017800u   /* canary (== the app's SP top)  */
#define STACK_GUARD_VALUE     0xCDCDCDCDu

/* ===================================================================== */
/* Image format                                                           */
/* ===================================================================== */
#define IMG_HEADER_OFFSET     0x200u        /* cleartext header @ slot+0x200 */
#define IMG_MAGIC             0xA1B2C3D4u    /* header word[0]                */
#define IMG_FOOTER_TAG        0x45444F43u    /* "CODE" (footer word[0])       */
#define IMG_MONOLITHIC_MAX    0x1C000u       /* monolithic path if len < this */
#define IMG_MAX_LEN           0x10000u       /* image_try_keys() length cap   */
#define SEG_TABLE_BYTES       0x2C0u         /* segment-table size (segmented)*/

/* The cipher copies words 128..143 (bytes 0x200..0x23F) verbatim.         */
#define IMG_HDR_WORD_FIRST    128u           /* 0x200 / 4                     */
#define IMG_HDR_WORD_LAST     143u           /* inclusive                     */
#define SEG_SKIP_WORDS        0x80u          /* skip_words arg for segments   */

/* xorshift128 key-whitening constant                                       */
#define PRNG_WHITEN_MASK      0x13579BDFu

/* image_try_keys() return codes                                            */
#define KEYID_NONE            0
#define KEYID_B_OR_DEVICE     1
#define KEYID_A               2

/* Flash-layer status codes (as returned by the disassembly)                */
#define FL_OK                 0
#define FL_VERIFY_FAIL        6              /* read-back / blank mismatch    */
#define FL_BADARG             11             /* 0xB: out of range / misalign  */
#define FL_NOTINIT            2

/* ===================================================================== */
/* Prototypes — names verbatim from the disassembly                       */
/* ===================================================================== */

/* --- startup_lpc43xx.c --- */
void      Reset_Handler(void);

/* --- boot_main.c --- */
void      boot_main(void) __attribute__((noreturn));
uint32_t  select_boot_slot(int update_flag);          /* returns slot base addr */
int       image_try_keys(const uint32_t *slot_base);  /* 0 / 1(KeyB) / 2(KeyA)  */
int       is_allowed_entry_addr(uint32_t addr);        /* 1 = SP plausible       */
void      memzero_words(void *dst, uint32_t byte_len);

/* --- crypto_stream.c --- */
uint32_t  xorshift128_next(uint32_t *state);
uint32_t *prng_seed_from_key(uint32_t *state, const uint32_t *key);
uint32_t *prng_seed_keyA(uint32_t *state);
uint32_t *prng_seed_keyB_or_device(uint32_t *state);
int       stream_decrypt_skip_header(void *dst, const uint32_t *src,
                                     uint32_t byte_len, uint32_t *state);
int       stream_checksum16(const uint32_t *src, uint32_t byte_len, uint32_t *state);
int       stream_decrypt_segment(void *dst, uint32_t image_base, uint32_t image_offset,
                                 uint32_t byte_len, uint32_t skip_words, uint32_t *state);

/* --- spifi_glue.c (bootloader wrappers around lpcspifilib) --- */
int       spifi_init(void);     /* init + register family + init device + XIP; refcounted */
int       spifi_deinit(void);   /* refcount-- ; restores XIP on last release               */
void      spifi_lock_acquire(void);  /* atomic refcount++, masks IRQs on 0->1              */
void      spifi_lock_release(void);  /* atomic refcount--, unmasks IRQs on 1->0            */

/* --- flash_if.c --- */
int       mem_read(void *dst, const void *src, uint32_t len);        /* memcpy + return 0  */
bool      mem_read_any(void *dst, const void *src, uint32_t len);    /* flash/RAM dispatch */
int       mem_cmp_any(const void *a, const void *b, uint32_t len);   /* 0 == equal         */
/* mem_is_filled: returns 1 if the region is NOT uniformly == fill_byte, else 0.
 * (Name reads backwards — it detects "differs from fill", used for blank-checks.) */
int       mem_is_filled(const void *addr, int fill_byte, uint32_t len);
int       flash_program(uint8_t *dst, const void *src, uint32_t len);
int       flash_program_rmw(uint32_t dst, const uint8_t *src, uint32_t len); /* src NULL = erase-fill */
/* flash_chip_erase: despite the name, erases the SINGLE 64K block containing addr
 * (unlock BP, set command mode, erase one block, verify all-0xFF). */
int       flash_chip_erase(uint8_t *addr);
int       buf_all_eq(const uint8_t *p, uint32_t len, int val);       /* 1 == all equal     */
/* dwords_all_eq: returns 0 if every dword equals val, nonzero otherwise
 * (opposite polarity to buf_all_eq — beware). */
int       dwords_all_eq(const void *p, uint32_t val, uint32_t len);

/* --- util_mem.c (these mirror the toolchain's __aeabi_* copy helpers) --- */
void     *memcpy_words(void *dst, const void *src, int len);
void     *memcpy_halfwords(void *dst, const void *src, int len);
void     *memcpy_auto(void *dst, const void *src, int len);          /* alignment dispatch */
void     *memset_bytes(void *dst, int val, int len);
int       memcmp_bytes(const void *a, const void *b, int len);
void     *memcpy_fast(void *dst, const void *src, uint32_t len);     /* unrolled bulk copy */

/* --- keys.c (placed at fixed addresses by the linker; see ld script) --- */
extern uint32_t      g_boot_config_base;            /* .data: value 0x14010000           */
extern uint8_t       g_keyA[16];                    /* .data: RAM copy of Key A           */
extern uint8_t       g_keyB[16];                    /* .data: RAM copy of Key B           */
extern const uint8_t g_device_key_slot_flash[16];   /* flash @0x14000218 (read at runtime)*/

#endif /* BOOTLOADER_H */