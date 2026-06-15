/* spifi_glue.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Interface to the IMPORTED bespoke SPIFI driver. The driver is a single
 * position-relocated code+data blob (it is NOT NXP lpcspifilib); this file does
 * not reimplement it — it only describes its request-struct ABI, reserves its
 * RAM context, and provides the three flash-resident thunk stubs that branch
 * into it. The actual driver bytes are imported (see the relocation note below).
 *
 * RELOCATION
 *   The driver blob is copied at boot from flash to SRAM by Reset_Handler's
 *   scatter-load (the .text_ram triplet):
 *       flash 0x14000BE8  ->  RAM 0x10010000 ,  length 0x1D70
 *   so for any RAM address R inside the relocated driver:
 *       flash = R - 0x10010000 + 0x14000BE8
 *   The boot logic stays in flash (XIP) and calls into the RAM copy through the
 *   three thunks below, because driving the controller in command mode (for
 *   program/erase) drops XIP and you cannot fetch the driver from flash then.
 *
 *   One driver helper is the reverse: memcpy_bytes_thunk (flash 0x14002948)
 *   relocates into RAM with the blob but BRANCHES BACK to the flash-resident
 *   byte-copy at 0x14000844 (memcpy_bytes, util_mem.c) — a RAM->flash
 *   cross-boundary call. It is part of the driver blob and is not reproduced
 *   here; it is noted in util_mem.c.
 *
 * The spifi_request_t ABI struct is declared in bootloader.h (so flash_if.c can
 * fill it); this file provides its instance, the driver context, and the thunks.
 */

#include "bootloader.h"

/* ABI struct (mirrors the declaration in bootloader.h). The driver reads this
 * to perform one program/erase operation. */
typedef struct {
    uint32_t flash_offset;   /* +0x00 byte offset from 0x14000000             */
    uint32_t length;         /* +0x04 byte count                              */
    uint32_t stage_buf;      /* +0x08 AHB staging buffer (0x20000000), or 0   */
    uint32_t sentinel;       /* +0x0C 0 / 0xFFFFFFFF                          */
    uint32_t opcode;         /* +0x10 SPIFI_OP_PROGRAM (8) / SPIFI_OP_ERASE   */
} spifi_request_t;

/* The boot loader's entire BSS (0x94 bytes at 0x10011ED0), zeroed by CRT0.
 * Declaration order places the request struct first (0x10011ED0) and the driver
 * context immediately after (0x10011EE4); the precise addresses are a linker
 * placement detail. */
spifi_request_t        g_spifi_request_obj;          /* 0x10011ED0, 20 bytes */
uint8_t                g_spifi_driver_ctx[0x80];      /* 0x10011EE4, ctx      */
spifi_request_t *const g_spifi_request = &g_spifi_request_obj;

/* Relocated driver entry points (RAM, Thumb). Offsets from 0x10010000:
 *   init    @ 0x100105FF : controller reset, JEDEC detect, vendor command-set
 *                          selection, read/quad mode config, enter XIP
 *   program @ 0x10010E95 : page-program loop per request
 *   op      @ 0x100110EB : erase per request                                  */
typedef int (*drv_init_fn)   (uint32_t ctrl_base, void *ctx);
typedef int (*drv_program_fn)(spifi_request_t *req, void *ctx);
typedef int (*drv_op_fn)     (spifi_request_t *req, void *ctx);

#define DRV_INIT_ENTRY     0x100105FFu
#define DRV_PROGRAM_ENTRY  0x10010E95u
#define DRV_OP_ENTRY       0x100110EBu

/* Flash-resident thunks (0x14000BB8 / 0x14000BC8 / 0x14000BD8). Each is a tiny
 * stub that branches into the relocated driver. They stay in flash so the boot
 * logic — itself running from flash — can reach the RAM driver across the XIP
 * boundary. */
int spifi_drv_init_thunk(uint32_t ctrl_base, void *ctx)
{
    return ((drv_init_fn)DRV_INIT_ENTRY)(ctrl_base, ctx);
}
int spifi_drv_program_thunk(spifi_request_t *req, void *ctx)
{
    return ((drv_program_fn)DRV_PROGRAM_ENTRY)(req, ctx);
}
int spifi_drv_op_thunk(spifi_request_t *req, void *ctx)
{
    return ((drv_op_fn)DRV_OP_ENTRY)(req, ctx);
}