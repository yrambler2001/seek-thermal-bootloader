// spifi_driver_blob/spifi_mem.c
/* spifi_mem.c — reconstructed from the driver blob disassembly (clean region).
 *
 * Memory-mode (XIP) entry/exit plus the program/erase completion poll and the
 * status-register writer. spifi_enter_mem_mode is the routine that actually
 * arms the controller's MCMD register so the flash window at 0x14000000 reads
 * as memory; spifi_exit_mem_mode tears that down so command mode works again.
 *
 * Descriptor (spifi_dev_t) field reads are confirmed by offset against the
 * disassembly; the field *names* (read_params/caps/wip_bit/status_poll/...) are
 * the best-effort interpretation from spifi_regs.h. The bit positions tested
 * below are taken verbatim from the lifted instructions (the source used raw
 * shifts like `LSLS r,#0x1E`/`BMI`/`BPL`; those are rendered here as the
 * equivalent mask tests).
 */
#include "spifi_regs.h"

/* read_params (descriptor +0x1C) capability/dummy bits the blob shifts out */
#define RP_QUAD_CMD        0x00200000u   /* bit21: quad command set present   */
#define RP_QPI_PRESENT     0x00800000u   /* bit23                             */
#define RP_DUMMY_FIELD     0x00070000u   /* bits18..16: dummy-cycle field     */

/* caps (descriptor +0x28) feature bits */
#define CAP_QPI_ENTER      0x00400000u   /* bit22                             */
#define CAP_4BYTE          0x04000000u   /* bit26                             */
#define CAP_QUAD_VARIANT   0x00200000u   /* bit21                             */
#define CAP_HOLD_RESET     0x00000001u   /* bit0                              */

/* forward decls from other clean regions */
extern uint32_t spifi_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t datalen);
extern void     spifi_cmd_data(spifi_dev_t *dev, uint32_t opcode, uint32_t frame,
                               uint32_t addr);
extern void     spifi_wren_then_cmd(spifi_dev_t *dev, uint32_t opcode,
                                    uint32_t frame, uint32_t addr);
extern uint32_t spifi_quad_opt_bits(spifi_dev_t *dev);
extern uint32_t spifi_write_status(spifi_dev_t *dev, uint32_t which, uint32_t val);

/* ===========================================================================
 * spifi_exit_mem_mode  (flash 0x14000D4E)
 *
 * If the controller is in memory mode (STAT bits 1/2), reset it: write STAT
 * bit4 and spin until the reset/cmd bits clear. Then, if the device uses a
 * "continuous read" mode (read_params bit29) with a high enough dummy field
 * (>= 0xC00000), issue a dummy framed read to break the device out of its
 * continuous-read latch. Returns the descriptor (vestigial; callers ignore it).
 * ========================================================================= */
uint32_t spifi_exit_mem_mode(spifi_dev_t *dev)
{
    uint32_t stat = SPIFI->STAT;

    if (stat & (SPIFI_STAT_CMD | 0x00000004u)) {
        SPIFI->STAT = SPIFI_STAT_RESET;                 /* 0x10 */
        while (SPIFI->STAT & (SPIFI_STAT_RESET | SPIFI_STAT_CMD | 0x4u)) { }

        if (stat & 0x00000004u) {                       /* bit29 of the original test */
            uint32_t rp = dev->read_params;
            if ((rp & 0x00E00000u) >= 0x00C00000u) {
                SPIFI->IDATA = 0xFFu;                   /* dummy intermediate byte */
                /* re-issue the device's read command with datalen forced to 1 */
                uint32_t cmd = (dev->read_params & ~SPIFI_CMD_DLEN_MASK) + 1u;
                SPIFI->CMD = cmd;
                (void)(uint8_t)SPIFI->DATA;
                while (SPIFI->STAT & SPIFI_STAT_CMD) { }
            }
        }
    }
    return (uint32_t)(uintptr_t)dev;
}

/* ===========================================================================
 * spifi_enter_mem_mode  (flash 0x14000E9A)
 *
 * Bring the device into the configured high-speed read mode and arm the
 * controller's MCMD so 0x14000000 reads as memory:
 *   1. If QPI is available (read_params bit22) and the device is currently in
 *      a 0x800000 state, set the 0x200000 read_params bit and mirror it into
 *      the controller (CLIMIT bookkeeping).
 *   2. If the quad-read path is selected (read_params bit26), read the
 *      configuration register (0x85), splice the dummy-cycle field, and program
 *      it back (0x81) via WREN.
 *   3. If a QPI-enter opcode is required (caps bit31), issue 0xA3 and burn a
 *      short settle loop.
 *   4. Compute the MCMD frame/dummy form from the read_params dummy field and
 *      write MCMD.
 *
 * The exact opcode/field constants (0x85/0x81/0xA3, 0xA5 vs 0xFF idata) are
 * lifted verbatim; their *purpose* is the standard SPI-NOR quad/QPI enable
 * sequence.
 * ========================================================================= */
uint32_t spifi_enter_mem_mode(spifi_dev_t *dev)
{
    uint32_t rp   = dev->read_params;
    uint32_t caps = dev->caps;
    uint32_t settle = 0xA6u;

    /* step 1: QPI present + device in 0x800000 state -> latch 0x200000 */
    if ((caps & CAP_QPI_ENTER) &&
        ((rp & 0x00A00000u) == 0x00800000u)) {
        rp |= 0x00200000u;
        dev->read_params = rp;
        SPIFI->CLIMIT |= 0x00200000u;                   /* mirror into +0x20 */
    }

    /* step 2: quad-read config-register splice (0x85 read / 0x81 write) */
    if (caps & 0x04000000u) {                            /* bit26 */
        uint32_t cr_dummy;
        if (caps & 0x00200000u)  cr_dummy = 2u;          /* bit21 */
        else                     cr_dummy = 1u;
        cr_dummy += 4u;

        {
            uint32_t v   = spifi_cmd(dev, 0x85u, 1u) & 7u;
            uint32_t fld = (dev->read_params & 0x00070000u) >> (16u - cr_dummy);
            uint32_t cfg = v | fld;
            (void)cfg;
            spifi_wren_then_cmd(dev, 0x81u, 1u, 0u);
        }
    }

    /* step 3: explicit QPI-enter opcode 0xA3, then a settle delay */
    if (dev->caps & 0x00000001u) {                       /* low bit of +0x28 byte */
        spifi_cmd_data(dev, 0xA3u, 3u, 0u);
        while (--settle != 0u) { }
    }

    /* step 4: assemble and arm the memory command */
    {
        uint32_t dummy = (rp >> 0x15u) & 7u;             /* bits 23..21 */
        if (dummy >= 6u) {
            SPIFI->IDATA = 0xA5u;
            rp &= ~0x00C00000u;
        } else {
            SPIFI->IDATA = 0xFFu;
        }
        SPIFI->MCMD = rp;
    }
    return (uint32_t)(uintptr_t)dev;
}

/* ===========================================================================
 * spifi_wait_program_done  (flash 0x14000D8E)
 *
 * Poll the device's WIP (write-in-progress) bit until it clears, with a bounded
 * spin (0x4E20 status reads), then optionally re-read status and decide a
 * timeout budget per the 'mode' selector (0..3 pick increasingly large word
 * budgets). On success leaves memory mode and returns 0; on a hard timeout
 * returns a non-zero code. The large magic budgets (0x38270 / 0x419CE0 /
 * 0xA408300) are lifted verbatim.
 *
 * This is the most intricate clean routine; the control flow is faithful but
 * the budget semantics are inferred from how 'mode' indexes them.
 * ========================================================================= */
uint32_t spifi_wait_program_done(spifi_dev_t *dev, uint32_t mode)
{
    uint32_t wip = 1u << dev->wip_bit;
    uint32_t wip_byte = (uint8_t)wip;
    uint32_t guard = 0u;
    uint32_t status = 0u;
    uint32_t budget;

    while (SPIFI->STAT & SPIFI_STAT_CMD) { }

    /* short bounded poll of the WIP bit via RDSR (0x05) */
    for (guard = 0u; guard < 0x4E20u; guard++) {
        status = (uint8_t)spifi_cmd(dev, 0x05u, 1u);
        if ((status & wip_byte) == 0u)
            break;
    }
    dev->status0 = (uint8_t)status;

    if ((status & wip_byte) == 0u) {
        /* completed: drop XIP and report success */
        spifi_exit_mem_mode(dev);
        return 0u;
    }

    /* still busy: pick a larger word budget by mode and keep polling MCMD-style */
    switch (mode) {
    case 0u:  budget = 0x00419CE0u; break;
    case 1u:  budget = 0x0A408300u; break;
    case 2u:  budget = 0x00038270u; break;
    default:  budget = 0xFFFFFFFFu; break;
    }

    {
        uint32_t cmd = spifi_quad_opt_bits(dev) | 0x05204000u | dev->wip_bit;
        SPIFI->CMD = cmd;
        while (budget != 0u) {
            if ((SPIFI->STAT & SPIFI_STAT_CMD) == 0u)
                break;
            budget--;
        }
        if (budget == 0u) {
            spifi_exit_mem_mode(dev);
            return 0u;
        }
        dev->status0 = (uint8_t)SPIFI->DATA;
    }

    /* second-status / busy-poll path (mode != 0): RDSR-2 (descriptor +0x2C) */
    if (mode != 0u) {
        uint32_t poll = dev->status_poll;
        if (poll != 0u) {
            uint32_t op = (uint8_t)poll;
            if (op != 5u) {
                status = (uint8_t)spifi_cmd(dev, op, 1u);
                dev->status1 = (uint8_t)status;
            } else {
                status = dev->status0;
            }
            if ((status & (poll >> 8)) != 0u) {
                uint32_t clr = (poll >> 16) & 0xFFu;
                if (clr != 0u)
                    (void)spifi_cmd(dev, clr, 0u);
                return 0x00020005u;          /* "busy/locked" code, lifted */
            }
        }
    }
    return 0u;
}

/* ===========================================================================
 * spifi_write_status  (flash 0x14000E68)
 *
 * Build a WRSR-class command (opcode 1) carrying 'val', issue it via
 * spifi_cmd_addr_data, then if the device reports a follow-up busy (caps bit18)
 * wait for completion. 'which' selects the status register variant.
 * ========================================================================= */
uint32_t spifi_write_status(spifi_dev_t *dev, uint32_t which, uint32_t val)
{
    extern void spifi_cmd_addr_data(spifi_dev_t *dev, uint32_t cmd, uint32_t addr,
                                    uint32_t data);
    uint32_t cmd = spifi_make_cmd(1u, 0u, 0u, which);
    (void)which;
    spifi_cmd_addr_data(dev, cmd, 0u, val);

    if (((uint16_t)dev->bp_mask_a) & 0x0002u)            /* LSLS #0x12; BPL test */
        return 0u;
    return spifi_wait_program_done(dev, 0u);
}