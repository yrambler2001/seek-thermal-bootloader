// spifi_driver_blob/spifi_probe.c
/* spifi_probe.c — reconstructed from the driver blob (MOST byte-derived file).
 *
 * The three relocated entry points the bootloader actually calls through its
 * flash thunks, plus the top-level probe/dispatch they sit on and the
 * cross-boundary memcpy helper.
 *
 *   RAM 0x100105FF  <- spifi_drv_init_thunk     : probe + configure + enter XIP
 *   RAM 0x10010E95  <- spifi_drv_program_thunk  : one program request
 *   RAM 0x100110EB  <- spifi_drv_op_thunk       : one erase request
 *
 * CONFIDENCE — READ THIS FIRST:
 *   These three routines live in the RELOCATED driver image (VMA 0x10010000+).
 *   The analysis disassembled the blob at its flash LMA, where this region is
 *   raw DCB bytes; the lifted, named routines in the other files are the parts
 *   that happened to decode. So everything in THIS file is reconstructed from
 *   the CALL GRAPH and the request-struct ABI (what each entry must read/do to
 *   satisfy flash_if.c and to drive the vendor configs), NOT from lifted
 *   instructions. Treat this file as a behavioural model. To build a runnable
 *   image, keep the original relocated blob bytes for these entries.
 *
 *   What IS firm (from flash_if.c / spifi_glue.c, confirmed):
 *     - the three entry addresses and that each takes one pointer arg (the
 *       driver context for init; the request struct for program/op),
 *     - the request-struct field map (+0x00 flash_offset, +0x04 length,
 *       +0x08 stage_buf, +0x0C sentinel, +0x10 opcode 8=program / 0x20=erase),
 *     - that init must end with the device in memory-mapped (XIP) mode,
 *     - that the program/erase entries return 0 on success (flash_if.c maps the
 *       return through CLZ>>5, i.e. "0 == ok").
 *   What is INFERRED: the internal sequencing below.
 *
 *   None of this is checkable against README §22: on this dump the part is
 *   already probed and in XIP before the bootloader's image_try_keys runs, so
 *   the init entry's result is only ever "stay in XIP", and no program/erase
 *   request is issued.
 */
#include "spifi_regs.h"

/* ---- the lifted/byte-derived helpers from the other driver files ---------- */
extern uint32_t spifi_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t datalen);
extern uint32_t spifi_exit_mem_mode(spifi_dev_t *dev);
extern uint32_t spifi_enter_mem_mode(spifi_dev_t *dev);
extern uint32_t spifi_erase_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t addr);
extern uint32_t spifi_program_region(spifi_dev_t *dev, const uint8_t *data,
                                     const void *req);
extern void    *memcpy_bytes(void *dst, const void *src, uint32_t len);

extern uint32_t spifi_cfg_micron(spifi_dev_t *dev, uint32_t mode);
extern uint32_t spifi_cfg_micron_2(spifi_dev_t *dev, uint32_t mode);
extern uint32_t spifi_cfg_winbond_spansion(spifi_dev_t *dev, uint32_t mode, uint32_t freq);
extern uint32_t spifi_cfg_sst(spifi_dev_t *dev, uint32_t mode);
extern uint32_t spifi_cfg_macronix(spifi_dev_t *dev, uint32_t mode, uint32_t freq);

/* The request struct flash_if.c marshals (instance @0x10011ED0). Mirrors the
 * confirmed ABI; this is what the program/op entries read. */
typedef struct {
    uint32_t flash_offset;   /* +0x00 */
    uint32_t length;         /* +0x04 */
    uint32_t stage_buf;      /* +0x08 */
    uint32_t sentinel;       /* +0x0C */
    uint32_t opcode;         /* +0x10 : 8 = program, 0x20 = erase */
} spifi_request_t;

/* default bus frequency the configs are tuned against (inferred) */
#define PROBE_FREQ   0x2Du

/* ===========================================================================
 * spifi_probe  (the body the init entry runs)   [BYTE-DERIVED / INFERRED]
 *
 * Reset the controller out of any prior XIP state, read the JEDEC ID (0x9F,
 * 3 bytes: manufacturer, type, capacity) into the descriptor, then dispatch to
 * the matching vendor config (which sets the read/dummy/erase fields and the XIP
 * read command). Finally enter memory-mapped mode so 0x14000000 reads as flash.
 *
 * The JEDEC opcode (0x9F) and the manufacturer dispatch values are the same ones
 * the vendor configs key on; the dispatch table here mirrors spifi_vendor.c.
 * Returns 0 on success.
 * ========================================================================= */
static uint32_t spifi_probe(spifi_dev_t *dev)
{
    uint32_t id, mfg;
    uint32_t rc;

    spifi_exit_mem_mode(dev);

    /* JEDEC Read-ID: 3 data bytes -> mfgr(+0x10), type(+0x11), capacity(+0x12) */
    id = spifi_cmd(dev, 0x9Fu, 3u);
    dev->mfgr_id      = (uint8_t)(id);
    dev->dev_type     = (uint8_t)(id >> 8);
    dev->dev_capacity = (uint8_t)(id >> 16);
    mfg = dev->mfgr_id;

    switch (mfg) {
    case 0x20u:                                   /* Micron / Spansion-alias */
        rc = spifi_cfg_micron(dev, PROBE_FREQ);
        if (rc != 0u)                             /* fall back to the alt decode */
            rc = spifi_cfg_winbond_spansion(dev, PROBE_FREQ, PROBE_FREQ);
        break;
    case 0xC2u:                                   /* Macronix */
        rc = spifi_cfg_macronix(dev, PROBE_FREQ, PROBE_FREQ);
        break;
    case 0xBFu:                                   /* SST */
        rc = spifi_cfg_sst(dev, PROBE_FREQ);
        break;
    case 0xEFu:  case 0xC8u:  case 0x9Du:         /* Winbond / GigaDevice / ISSI */
    case 0x40u:  case 0x80u:  case 0x71u:
        rc = spifi_cfg_winbond_spansion(dev, PROBE_FREQ, PROBE_FREQ);
        break;
    default:
        rc = spifi_cfg_micron_2(dev, PROBE_FREQ); /* last-ditch generic decode */
        break;
    }

    if (rc == 0u)
        spifi_enter_mem_mode(dev);                /* leave the part in XIP */
    return rc;
}

/* ===========================================================================
 * drv_init_entry  (RAM 0x100105FF)   [BYTE-DERIVED / INFERRED]
 *
 * The target of spifi_drv_init_thunk. flash_if.c's spifi_init pin-muxes the bus,
 * primes the request struct, and calls this with the driver context. It zero-
 * initialises the descriptor's transient fields and runs the probe.
 * Returns 0 on success (flash_if.c treats 0 as ok via CLZ>>5).
 * ========================================================================= */
int drv_init_entry(void *ctx)
{
    spifi_dev_t *dev = (spifi_dev_t *)ctx;

    /* clear the transient/scratch descriptor fields the probe will fill */
    dev->sfdp_src          = NULL;       /* +0x38: no SFDP yet                */
    dev->caps              = 0u;         /* +0x28                              */
    dev->read_params       = 0u;         /* +0x1C                             */
    dev->cmd_base          = 0u;         /* +0x20                             */
    dev->sfdp_read_count   = 0u;
    dev->sfdp_param_dwords = 0u;

    return (int)spifi_probe(dev);
}

/* ===========================================================================
 * drv_program_entry  (RAM 0x10010E95)   [BYTE-DERIVED / INFERRED]
 *
 * The target of spifi_drv_program_thunk. Reads the marshalled request and
 * programs 'length' bytes at 'flash_offset' from the staging buffer, via
 * spifi_program_region. The op_flags spifi_program_region expects are
 * synthesised here from the request (program opcode, verify off — matching the
 * "no read-back verify" behaviour flash_if.c documents). Returns 0 on success.
 * ========================================================================= */
int drv_program_entry(void *req_ptr)
{
    spifi_request_t *req = (spifi_request_t *)req_ptr;
    spifi_dev_t     *dev = (spifi_dev_t *)RAM_DRIVER_CONTEXT;   /* 0x10011EE4 */

    /* build the 0x14-byte region request spifi_program_region copies in:
     *   start = flash_offset, length, src = stage_buf, fill = 0, flags = program */
    uint32_t region[5];
    region[0] = req->flash_offset;
    region[1] = req->length;
    region[2] = req->stage_buf;
    region[3] = 0u;
    region[4] = 0u;                       /* op_flags: program, no verify       */

    return (int)spifi_program_region(dev, (const uint8_t *)(uintptr_t)req->stage_buf,
                                     region);
}

/* ===========================================================================
 * drv_op_entry  (RAM 0x100110EB)   [BYTE-DERIVED / INFERRED]
 *
 * The target of spifi_drv_op_thunk. Reads the request and performs an erase of
 * 'length' bytes at 'flash_offset' (the bootloader only ever uses this for the
 * fixed 64 KB block erases in flash_chip_erase / flash_erase_region). Chooses
 * the descriptor's erase opcode and steps block-by-block. Returns 0 on success.
 * ========================================================================= */
int drv_op_entry(void *req_ptr)
{
    spifi_request_t *req = (spifi_request_t *)req_ptr;
    spifi_dev_t     *dev = (spifi_dev_t *)RAM_DRIVER_CONTEXT;
    uint32_t off = req->flash_offset;
    uint32_t len = req->length;
    uint32_t rc  = 0u;

    if (req->opcode != 0x20u)             /* only erase is defined here */
        return 0;

    spifi_exit_mem_mode(dev);
    while (len != 0u) {
        uint32_t blk = 1u << dev->size_shift;            /* erase granularity */
        rc = spifi_erase_cmd(dev, dev->erase_opcode, off);
        if (rc != 0u)
            break;
        off += blk;
        len  = (len > blk) ? (len - blk) : 0u;
    }
    spifi_enter_mem_mode(dev);
    return (int)rc;
}

/* ===========================================================================
 * memcpy_bytes_thunk  (flash 0x14002948)   [byte-derived; cross-boundary]
 *
 * Relocates into RAM with the blob but BRANCHES BACK to the flash-resident
 * memcpy_bytes (0x14000844, util_mem.c). spifi_program_region uses it to pull
 * the caller request into a local. Reproduced here as a one-line forwarder; in
 * the binary it is a tiny veneer (PUSH/load-literal/BX) — the original bytes are
 * what actually live at 0x14002948.
 * ========================================================================= */
void *memcpy_bytes_thunk(void *dst, const void *src, uint32_t len)
{
    return memcpy_bytes(dst, src, len);      /* RAM -> flash call, by design */
}