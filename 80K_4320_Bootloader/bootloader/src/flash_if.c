/* flash_if.c — 80K_4320_Bootloader (reconstructed)
 *
 * The flash-access layer the boot logic sits on: address-space-aware copy and
 * compare, a blank check, and program/erase built on lpcspifilib (IMPORTED).
 * Transfers touching the memory-mapped SPI-NOR window (top byte 0x14) are routed
 * through the driver; pure-RAM transfers go through the fast mem helpers.
 * Names are kept identical to the disassembly.
 */

#include "bootloader.h"
#include "spifilib_api.h"
#include "chip.h"

extern void *g_spifi_handle;   /* IDA: g_spifi_bss_start (defined in spifi_glue.c) */

/* True when an address is in the memory-mapped SPIFI window (0x14xxxxxx). */
#define IS_XIP(p)   ((((uint32_t)(uintptr_t)(p)) & 0xFF000000u) == SPIFI_XIP_BASE)

/* RMW staging-buffer base. The disassembly reads this pointer from a fixed cell
 * at 0x20000000 (AHB SRAM); whatever populates it is outside this image, so we
 * mirror the indirection rather than guess a buffer size. */
#define g_rmw_buf   (*(uint8_t * volatile *)0x20000000u)

/* ---------------------- trivial copy / compare ------------------------- */
int mem_read(void *dst, const void *src, uint32_t len)
{
    memcpy_auto(dst, src, len);
    return FL_OK;
}

int buf_all_eq(const uint8_t *p, uint32_t len, int val)
{
    const uint8_t *end = p + len;
    while (p != end)
        if (*p++ != (uint8_t)val)
            return 0;
    return 1;
}

/* Returns 0 if every dword in [p,p+len) equals val; otherwise the (nonzero)
 * difference of the first mismatch. NOTE: opposite polarity to buf_all_eq. */
int dwords_all_eq(const void *p, uint32_t val, uint32_t len)
{
    const uint32_t *cur = (const uint32_t *)p;
    const uint32_t *end = (const uint32_t *)((const uint8_t *)p + len);
    int diff = 0;
    while (cur != end) {
        diff = (int)(*cur - val);
        if (diff != 0)
            break;
        cur++;
    }
    return diff;
}

/* ----------------------- address-aware dispatch ------------------------ */
bool mem_read_any(void *dst, const void *src, uint32_t len)
{
    if (IS_XIP(src)) {                        /* source is flash */
        if (!IS_XIP(dst))
            return mem_read(dst, src, len) == 0;
        return false;                          /* flash -> flash: unsupported */
    }
    if (IS_XIP(dst))                           /* dest is flash, source is RAM */
        return flash_program_rmw((uint32_t)(uintptr_t)dst, (const uint8_t *)src, len) == 0;
    memcpy_fast(dst, src, len);                /* RAM -> RAM */
    return true;
}

int mem_cmp_any(const void *a, const void *b, int len)
{
    const uint8_t *flash_side, *ram_side;
    uint8_t buf[64];
    int rem, result = 0;

    if (IS_XIP(a)) {
        if (IS_XIP(b))
            return -1;                         /* flash vs flash: unsupported */
        flash_side = (const uint8_t *)a;
        ram_side   = (const uint8_t *)b;
    } else {
        if (!IS_XIP(b))
            return memcmp_bytes(a, b, len);    /* neither flash */
        flash_side = (const uint8_t *)b;       /* swap: flash side gets chunk-read */
        ram_side   = (const uint8_t *)a;
    }

    rem = len;
    do {
        int chunk = (rem >= 64) ? 64 : rem;
        int off   = len - rem;
        mem_read(buf, &flash_side[off], (uint32_t)chunk);
        result = memcmp_bytes(buf, &ram_side[off], chunk);
        if (result)
            break;
        rem -= chunk;
    } while (rem > 0);
    return result;
}

/* Returns 1 if the region contains any byte != fill_byte, else 0.
 * (Flash side read in 64-byte chunks; the name reads backwards.) */
int mem_is_filled(const void *addr, int fill_byte, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)addr;
    if (IS_XIP(addr)) {
        uint8_t buf[64];
        int rem = (int)len;
        for (;;) {
            int chunk = (rem >= 64) ? 64 : rem;
            int off   = (int)len - rem;
            mem_read(buf, &p[off], (uint32_t)chunk);
            for (int i = 0; i < chunk; i++)
                if (buf[i] != (uint8_t)fill_byte)
                    return 1;
            rem -= chunk;
            if (rem <= 0)
                return 0;
        }
    } else {
        const uint8_t *end = p + len;
        while (p != end)
            if (*p++ != (uint8_t)fill_byte)
                return 1;
        return 0;
    }
}

/* ------------------------- program / erase ----------------------------- */
/* Program len bytes at a flash address. Bounds-checked to one 64K block; src
 * must be word-aligned. If the target is not already erased, an RMW erase runs
 * first; then the data is programmed and read-back verified. */
int flash_program(uint8_t *dst, const void *src, uint32_t len)
{
    SPIFI_HANDLE_T *h;
    int blank, rc;

    if (((uint32_t)len + ((uint32_t)(uintptr_t)dst & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;
    if (((uint32_t)(uintptr_t)src & 3u) != 0u)
        return FL_BADARG;

    blank = buf_all_eq(dst, len, 0xFF);        /* target already erased? */

    spifi_lock_acquire();
    rc = spifi_init();
    if (rc) goto unlock;

    h  = (SPIFI_HANDLE_T *)g_spifi_handle;
    rc = spifiDevSetMemMode(h, false);                          /* IDA: spifi_set_memmode (command) */
    if (!rc) {
        if (blank || (rc = flash_program_rmw((uint32_t)(uintptr_t)dst, NULL, len)) == 0) {
            rc = spifiProgram(h, (uint32_t)(uintptr_t)dst,
                              (const uint32_t *)src, len);      /* IDA: spifi_program */
            if (!rc) {
                rc = spifiDevSetMemMode(h, true);               /* restore XIP */
                if (!rc) {
                    rc = spifi_deinit();
                    if (!rc) {
                        if (mem_cmp_any(dst, src, len)) {       /* verify read-back */
                            spifi_lock_release();
                            return FL_VERIFY_FAIL;
                        }
                    }
                    spifi_lock_release();
                    return rc;
                }
                goto deinit;
            }
        }
        spifiDevSetMemMode(h, true);                            /* restore XIP after a failure */
    }
deinit:
    spifi_deinit();
unlock:
    spifi_lock_release();
    return rc;
}

/* Read-modify-write across erase blocks using the staging buffer. src == NULL
 * means "erase-fill" (used by flash_program to clear a target). */
int flash_program_rmw(uint32_t a1, const uint8_t *a2, uint32_t a3)
{
    SPIFI_HANDLE_T *h;
    uint32_t remaining = a3;
    uint32_t cur_addr  = a1;
    const uint8_t *cur_src = a2;
    uint32_t info;
    int rc;

    if (((uint32_t)a3 + (a1 & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;

    spifi_lock_acquire();
    rc = spifi_init();
    if (rc) goto unlock;

    h    = (SPIFI_HANDLE_T *)g_spifi_handle;
    info = spifiDevGetInfo(h, SPIFI_INFO_ERASE_BLOCKSIZE);      /* IDA: spifi_get_info(h,3) */
    if (info > 0x10000u) goto deinit_badarg;

    rc = spifiDevSetMemMode(h, false);                          /* command mode */
    if (rc) goto deinit;

    rc = spifiDevUnlockDevice(h);                               /* IDA: (*h)[0](h,0,0) unlock block-protect */
    if (rc) goto restore_deinit;

    do {
        uint32_t blk   = spifiGetBlockFromAddr(h, cur_addr);    /* IDA: spifi_get_block_from_addr */
        uint32_t base  = spifiGetAddrFromBlock(h, blk);          /* IDA: spifi_get_addr_from_block */
        uint32_t off   = cur_addr - base;
        uint32_t chunk = info - off;
        if (chunk >= remaining)
            chunk = remaining;

        if (off != 0u || info != chunk) {
            /* partial block: read whole block, patch, erase, reprogram */
            rc = spifiRead(h, base, (uint32_t *)g_rmw_buf, info);   /* IDA: spifi_read */
            if (rc) goto restore_deinit;

            if (cur_src) {
                memcpy_auto(g_rmw_buf + off, cur_src, (int)chunk);
                cur_src += chunk;
            } else {
                memset_bytes(g_rmw_buf + off, 0xFF, (int)chunk);    /* erase-fill */
            }

            rc = spifiErase(h, blk, 1);                             /* IDA: spifi_erase */
            if (rc || (rc = spifiProgram(h, base, (const uint32_t *)g_rmw_buf, info)) != 0)
                goto restore_deinit;
        } else {
            /* full block: erase, then program directly from the source */
            rc = spifiErase(h, blk, 1);
            if (rc) goto restore_deinit;
            if (((uint32_t)(uintptr_t)cur_src & 3u) != 0u)
                goto restore_deinit;                                /* src misaligned: bail (rc==0) */
            rc = spifiProgram(h, base, (const uint32_t *)cur_src, info);
            if (rc) goto restore_deinit;
            cur_src += info;
        }

        if (remaining < chunk) {                                    /* underflow guard */
            spifiDevSetMemMode(h, true);
            goto deinit_badarg;
        }
        remaining -= chunk;
        cur_addr  += chunk;
    } while (remaining);

    rc = spifiDevSetMemMode(h, true);                              /* restore XIP */
    if (rc) { spifi_deinit(); spifi_lock_release(); return rc; }
    rc = spifi_deinit();
    if (rc) { spifi_lock_release(); return rc; }
    if (mem_cmp_any((const void *)(uintptr_t)a1, a2, 0)) {         /* NB len 0: this verify is a no-op */
        spifi_lock_release();
        return FL_VERIFY_FAIL;
    }
    spifi_lock_release();
    return FL_OK;

restore_deinit:                 /* restore XIP, then deinit + unlock, return rc */
    spifiDevSetMemMode(h, true);
deinit:                         /* deinit + unlock, return rc (cmd-mode-fail path: no restore) */
    spifi_deinit();
    goto unlock;
deinit_badarg:                  /* deinit + unlock, return BADARG */
    spifi_deinit();
    spifi_lock_release();
    return FL_BADARG;
unlock:
    spifi_lock_release();
    return rc;
}

/* Despite the name, erases the SINGLE 64K block containing addr and verifies it
 * reads back all-0xFF. addr must be 64K-aligned. */
int flash_chip_erase(uint8_t *addr)
{
    SPIFI_HANDLE_T *h;
    uint32_t blk;
    int rc, rc3, rc5;

    if (((uint32_t)(uintptr_t)addr & 0xFFFFu) != 0u)
        return FL_BADARG;

    spifi_lock_acquire();
    rc = spifi_init();
    if (rc) goto unlock;                                          /* init failed */

    h  = (SPIFI_HANDLE_T *)g_spifi_handle;
    rc = spifiDevUnlockDevice(h);                                 /* IDA: (*h)[0](h,0,0) */
    if (rc) {                                                     /* unlock failed */
        spifiDevSetMemMode(h, true);
        spifi_deinit();
        goto unlock;
    }

    rc3 = spifiDevSetMemMode(h, false);                           /* command mode */
    if (rc3 == 0) {
        blk = spifiGetBlockFromAddr(h, (uint32_t)(uintptr_t)addr);
        rc3 = spifiErase(h, blk, 1);                              /* erase the one 64K block */
        if (rc3 == 0) {
            rc5 = spifiDevSetMemMode(h, true);                    /* restore XIP */
            if (rc5 || (rc5 = spifi_deinit()) != 0) {
                spifi_deinit();
                spifi_lock_release();
                return rc5;
            }
            if (mem_is_filled(addr, 0xFF, 0x10000)) {             /* not all-0xFF -> erase failed */
                spifi_lock_release();
                return FL_VERIFY_FAIL;
            }
            goto unlock;                                          /* success: return rc (==0) */
        }
        spifiDevSetMemMode(h, true);                              /* erase failed: restore + deinit */
        spifi_deinit();
    }
    spifi_lock_release();                                         /* cmd-mode-fail OR erase-fail */
    return rc3;

unlock:
    spifi_lock_release();
    return rc;
}