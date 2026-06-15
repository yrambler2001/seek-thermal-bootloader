/* flash_if.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * The flash-access layer the boot pipeline sits on. Unlike the iOS image (which
 * wrapped NXP lpcspifilib), this image drives a bespoke hand-written SPIFI
 * driver — see spifi_glue.c — which is IMPORTED, not reimplemented. This file
 * only marshals work into the driver's fixed request struct and calls its three
 * relocated entry points (the thunks). Names are kept identical to the
 * disassembly.
 *
 * Request-struct ABI (struct in spifi_glue.c, instance @0x10011ED0):
 *   +0x00 flash_offset : byte offset of the target from 0x14000000
 *   +0x04 length       : byte count
 *   +0x08 stage_buf    : AHB staging buffer (0x20000000) for program data, or 0
 *   +0x0C sentinel     : 0 / 0xFFFFFFFF
 *   +0x10 opcode       : 8 = program, 0x20 = erase
 * The driver context lives immediately after, at 0x10011EE4.
 *
 * The request ABI carries NO source pointer, so all program data is staged
 * through the AHB buffer at 0x20000000 first. Reads are plain XIP loads (the
 * flash is memory-mapped at 0x14000000), so the read-modify-write path needs no
 * driver "read" call. This build does NO read-back verify on program and NO
 * blank-verify on erase.
 */

#include "bootloader.h"

/* SCU pin-config registers for P3_x (SPIFI bus). Base 0x40086000, the SFSP3
 * group at +0x180, one 32-bit reg per pin. */
#define SCU_SFSP3(pin)   (*(volatile uint32_t *)(0x40086000u + 0x180u + ((pin) * 4u)))

/* ------------------------------ bring-up ------------------------------- */
int spifi_init(void)
{
    /* Pin-mux the SPIFI bus. Faithful oddities preserved from the machine code:
     *  - P3_3 is NOT configured. (The iOS image set P3_3..P3_8.)
     *  - P3_4 is written TWICE: 0xF3, then immediately 0xD3.
     * P3_5..P3_8 follow the standard SPIFI mux (function 3).                    */
    SCU_SFSP3(4) = 0xF3u;        /* first write (oddity)                        */
    SCU_SFSP3(4) = 0xD3u;        /* immediately overwritten                     */
    SCU_SFSP3(5) = 0xD3u;
    SCU_SFSP3(6) = 0xD3u;
    SCU_SFSP3(7) = 0xD3u;
    SCU_SFSP3(8) = 0x13u;

    /* Bring the imported driver up: probe the JEDEC ID, select the vendor
     * command set, configure read/quad modes, and enter memory-mapped (XIP)
     * mode. All of that lives behind the init thunk. */
    return spifi_drv_init_thunk(SPIFI_CTRL_BASE, g_spifi_driver_ctx);
}

/* ----------------------------- program --------------------------------- */
/* Program len bytes at a flash address. Bounds-checked to one 64K block; src
 * must be word-aligned. The data is staged into the AHB buffer (the request ABI
 * has no source field), then the program thunk is invoked. No read-back verify. */
int flash_program(uint8_t *dst, const void *src, uint32_t len)
{
    if (((uint32_t)len + ((uint32_t)(uintptr_t)dst & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;
    if (((uint32_t)(uintptr_t)src & 3u) != 0u)
        return FL_BADARG;

    /* marshal the source through the staging buffer (skip a self-copy) */
    if ((uintptr_t)src != STAGING_BUF_BASE)
        memcpy_auto((void *)STAGING_BUF_BASE, src, len);

    g_spifi_request->flash_offset = (uint32_t)(uintptr_t)dst - SPIFI_XIP_BASE;
    g_spifi_request->length       = len;
    g_spifi_request->stage_buf    = STAGING_BUF_BASE;     /* 0x20000000 */
    g_spifi_request->sentinel     = 0xFFFFFFFFu;
    g_spifi_request->opcode       = SPIFI_OP_PROGRAM;     /* 8 */
    return spifi_drv_program_thunk(g_spifi_request, g_spifi_driver_ctx);
}

/* ------------------------------- erase --------------------------------- */
/* Erase a range. Bounds-checked to one 64K block; no blank-verify. */
int flash_erase_region(uint32_t addr, uint32_t len)
{
    if (((uint32_t)len + (addr & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;

    g_spifi_request->flash_offset = addr - SPIFI_XIP_BASE;
    g_spifi_request->length       = len;
    g_spifi_request->stage_buf    = 0u;                   /* no data for erase */
    g_spifi_request->sentinel     = 0xFFFFFFFFu;
    g_spifi_request->opcode       = SPIFI_OP_ERASE;       /* 0x20 */
    return spifi_drv_op_thunk(g_spifi_request, g_spifi_driver_ctx);
}

/* Despite the name, erases the SINGLE 64K block containing addr — the length is
 * the fixed 0x10000, not the whole device. The 64K-alignment check is the guard;
 * this routine does NO blank-verify. */
int flash_chip_erase(uint8_t *addr)
{
    if (((uint32_t)(uintptr_t)addr & 0xFFFFu) != 0u)
        return FL_BADARG;
    return flash_erase_region((uint32_t)(uintptr_t)addr, 0x10000u);
}

/* ----------------------- read-modify-write ----------------------------- */
/* Patch a sub-block span without disturbing the rest of its 64K block: read the
 * whole block via XIP into the staging buffer, patch it, erase the block, and
 * reprogram it. src==NULL means erase-fill the span with 0xFF. (Used by
 * select_boot_slot to flip the 4-byte config selector.) No final verify. */
int flash_program_rmw(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint8_t  *stage = (uint8_t *)STAGING_BUF_BASE;
    uint32_t  block = addr & ~0xFFFFu;            /* 64K-aligned block base */
    uint32_t  off   = addr - block;
    int rc;

    if (((uint32_t)len + (addr & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;

    memcpy_auto(stage, (const void *)block, 0x10000u);    /* XIP read whole block */
    if (src)
        memcpy_auto(stage + off, src, len);               /* patch                */
    else
        for (uint32_t i = 0; i < len; i++) stage[off + i] = 0xFFu;  /* erase-fill */

    rc = flash_erase_region(block, 0x10000u);
    if (rc)
        return rc;
    return flash_program((uint8_t *)block, stage, 0x10000u);
}