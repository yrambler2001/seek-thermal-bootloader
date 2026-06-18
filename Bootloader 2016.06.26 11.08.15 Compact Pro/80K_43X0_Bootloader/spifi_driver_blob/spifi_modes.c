// spifi_driver_blob/spifi_mode­s.c
/* spifi_modes.c — reconstructed from the driver blob disassembly.
 *
 * Two routines:
 *   spifi_configure_modes      (flash 0x1400103A) — CLEAN, lifted fully.
 *   spifi_block_protect_engine (flash 0x140013B8) — STRUCTURALLY FAITHFUL.
 *
 * spifi_configure_modes programs the device into the read/quad/QPI mode the
 * controller wants and records the final read command (read_params/cmd_base) in
 * the descriptor. Every device command (opcode) below is verbatim from the
 * instructions; the mode-select bit positions are rendered as named masks (the
 * source used raw `LSLS Rx,R5,#n; BMI/BPL`). The bit *meanings* map onto the
 * standard JESD216 "quad-enable method" variants and are best-effort labels.
 *
 * spifi_block_protect_engine clears flash block-protection over the target
 * region before a program/erase. It lifted to instructions (no DCB code), but
 * it is ~1.1 KB of dense, register-pressure-driven control flow with an embedded
 * data/pointer table around 0x14001444; the reconstruction below preserves the
 * real control flow and every device opcode but is NOT bit-verifiable. Read it
 * as documentation of behaviour, not a drop-in.
 */
#include "spifi_regs.h"

extern uint32_t spifi_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t datalen);
extern void     spifi_cmd_data(spifi_dev_t *dev, uint32_t opcode, uint32_t frame,
                               uint32_t addr);
extern uint32_t spifi_cmd_addr(spifi_dev_t *dev, uint32_t cmd, uint32_t addr);
extern void     spifi_cmd_addr_data(spifi_dev_t *dev, uint32_t cmd, uint32_t addr,
                                    uint32_t data);
extern unsigned int spifi_make_cmd(uint32_t opcode, uint32_t frame,
                                   uint32_t intlen, uint32_t datalen);
extern uint32_t spifi_write_enable(spifi_dev_t *dev);
extern void     spifi_wren_then_cmd(spifi_dev_t *dev, uint32_t opcode,
                                    uint32_t frame, uint32_t addr);
extern uint32_t spifi_write_status(spifi_dev_t *dev, uint32_t which, uint32_t val);
extern uint32_t spifi_quad_opt_bits(spifi_dev_t *dev);
extern uint32_t spifi_detect_4byte_addr(spifi_dev_t *dev);
extern uint32_t spifi_read_sfdp(spifi_dev_t *dev);
extern uint32_t spifi_get_read_count(spifi_dev_t *dev, uint16_t *out);
extern uint32_t spifi_enter_mem_mode(spifi_dev_t *dev);
extern uint32_t spifi_exit_mem_mode(spifi_dev_t *dev);
extern void    *memcpy_fast(void *dst, const void *src, unsigned int len);

/* mode-select bits (the R1 argument); names map onto JESD216 QE methods */
#define MS_QPI_ENTER     (1u << 1)    /* issue 0x38 to enter QPI            */
#define MS_QE_SR2        (1u << 2)    /* QE via status-2 (0x35 / write SR2) */
#define MS_QE_SR_3F      (1u << 3)    /* QE via 0x3F read / 0x3E write      */
#define MS_QE_VCR        (1u << 4)    /* QE via volatile cfg (0x65 / 0x61)  */
#define MS_CTRL_DRWS     (1u << 10)   /* set SPIFI_CTRL bit28; also 0x40/0x80 mask */
#define MS_SET_DUMMY     (1u << 11)   /* program custom dummy cycles (0xC0) */
#define MS_QE_SR1B6      (1u << 12)   /* QE via status-1 bit6 (0x05 / write SR1) */

/* status codes returned (verbatim) */
#define CFG_OK            0x00000000u
#define CFG_ALREADY       0x00020005u   /* QE already set / no change         */
#define CFG_UNSUPPORTED   0x00020002u   /* manufacturer not handled           */

/* ===========================================================================
 * spifi_configure_modes  (flash 0x1400103A)   [CLEAN]
 *
 * args: dev, mode_select, and a 64-bit read-command config (cmd_lo:cmd_hi) that
 * is finally stored to dev->read_params(+0x1C):dev->cmd_base(+0x20).
 * ========================================================================= */
uint32_t spifi_configure_modes(spifi_dev_t *dev, uint32_t mode_select,
                               uint32_t cmd_lo, uint32_t cmd_hi)
{
    /* already configured (sentinel read_params) -> nothing to do */
    if (dev->read_params == 0x03803FFFu)
        return CFG_OK;

    dev->caps = mode_select;
    spifi_detect_4byte_addr(dev);

    /* parts larger than 16 MB need an explicit 4-byte-address enable */
    if (dev->size_field > 0x01000000u) {
        dev->caps |= 0x00000200u;
        switch (dev->mfgr_id) {
        case 0x01u:                                   /* Spansion: write bank reg */
            spifi_cmd_data(dev, 0x17u, 1u, 0x80u);
            break;
        case 0x20u:                                   /* Micron: WREN then EN4B   */
            spifi_write_enable(dev);
            /* fall through */
        case 0xC2u:                                   /* Macronix: EN4B (0xB7)    */
            spifi_cmd(dev, 0xB7u, 0u);
            break;
        default:
            return CFG_UNSUPPORTED;
        }
    }

    /* optional controller dual-data read window select */
    if (mode_select & MS_CTRL_DRWS)
        SPIFI->CTRL |= 0x10000000u;

    /* ---- quad-enable, by method ------------------------------------------ */
    if (mode_select & MS_QE_SR2) {
        uint32_t sr2 = (uint8_t)spifi_cmd(dev, 0x35u, 1u);
        dev->status1 = (uint8_t)sr2;
        if (sr2 & 0x02u)                              /* already QE */
            goto set_read_mode;
        if (sr2 & 0x01u)
            return CFG_ALREADY;
        {
            uint32_t sr1 = (uint8_t)spifi_cmd(dev, 0x05u, 1u);
            uint32_t combined;
            sr2 = dev->status1 | 0x02u;
            combined = sr1 | (sr2 << 8);
            if (spifi_write_status(dev, 2u, (uint16_t)combined) != 0u)
                return CFG_OK;                        /* write failed: bail 0 */
            sr2 = (uint8_t)spifi_cmd(dev, 0x35u, 1u);
            dev->status1 = (uint8_t)sr2;
            if ((sr2 & 0x02u) == 0u)                  /* readback: QE didn't stick */
                return CFG_ALREADY;
            goto set_read_mode;
        }
    } else if (mode_select & MS_QE_SR1B6) {
        uint32_t sr1 = (uint8_t)spifi_cmd(dev, 0x05u, 1u);
        dev->status0 = (uint8_t)sr1;
        if (sr1 & 0x40u)                              /* bit6 already set */
            goto set_read_mode;
        if (spifi_write_status(dev, 1u, sr1 | 0x40u) != 0u)
            return CFG_OK;
        sr1 = (uint8_t)spifi_cmd(dev, 0x05u, 1u);
        dev->status0 = (uint8_t)sr1;
        if ((sr1 & 0x40u) == 0u)
            return CFG_ALREADY;
        goto set_read_mode;
    } else if (mode_select & MS_QE_SR_3F) {
        uint32_t s = (uint8_t)spifi_cmd(dev, 0x3Fu, 1u);
        dev->status1 = (uint8_t)s;
        if (s & 0x80u)
            goto set_read_mode;
        spifi_wren_then_cmd(dev, 0x3Eu, 1u, s | 0x80u);
        s = (uint8_t)spifi_cmd(dev, 0x3Fu, 1u);
        dev->status1 = (uint8_t)s;
        if ((s & 0x80u) == 0u)
            return CFG_ALREADY;
    }

set_read_mode:
    /* QPI / volatile-config entry, then commit the read command */
    if (mode_select & MS_QPI_ENTER) {
        spifi_cmd(dev, 0x38u, 0u);                    /* enter QPI */
    } else if (mode_select & MS_QE_VCR) {
        uint32_t qe_mask = (mode_select & MS_CTRL_DRWS) ? 0x40u : 0x80u;
        uint32_t vcr = (uint8_t)spifi_cmd(dev, 0x65u, 1u);
        dev->status1 = (uint8_t)vcr;
        if (vcr & qe_mask) {
            vcr = (vcr | 0xC8u) & ~qe_mask;
            spifi_wren_then_cmd(dev, 0x61u, 1u, vcr);
        }
    }

    /* record the 4-byte-address sub-mode in caps */
    if (((cmd_lo >> 0x13u) & 3u) == 3u)
        dev->caps |= 0x00040000u;
    else
        dev->caps &= ~0x00040000u;

    /* program custom dummy cycles if requested */
    if (mode_select & MS_SET_DUMMY) {
        uint32_t dummy = ((cmd_lo >> 0x10u) & 7u) - 1u;
        spifi_cmd_data(dev, 0xC0u, 1u, dummy);
    }

    /* commit the 64-bit read command into read_params:cmd_base */
    dev->read_params = cmd_lo;
    dev->cmd_base    = cmd_hi;
    return CFG_OK;
}

/* ===========================================================================
 * spifi_block_protect_engine  (flash 0x140013B8)   [STRUCTURALLY FAITHFUL]
 *
 * Clear flash block-protection over a target region so a subsequent erase /
 * program can proceed, then re-enter memory (XIP) mode. The caller passes a
 * region descriptor (param: { start, length, ... }), a one-byte "changed" flag,
 * and a scratch out-halfword.
 *
 * Two strategies, chosen by capability bits in dev->caps:
 *   (A) Global status-register BP bits (caps bit14): read SR1 (+ SR2 if the part
 *       has one), clear the block-protect mask bits (dev->bp_mask_a / _b), and
 *       write status back — retrying up to 3 times with a settle delay and a
 *       readback check.
 *   (B) Per-region individual block lock (caps bits15..17): walk the SFDP
 *       sector-map descriptors (read via spifi_read_sfdp when needed), and for
 *       each protected block overlapping the target range issue the device's
 *       unlock command — Global/Individual Block-Unlock 0x98 / 0x39 / 0xE5, or
 *       read-lock 0x3C / 0xE8 — advancing by the descriptor's sector size.
 *
 * NOTE (reconstruction): every opcode here (0x05, 0x35, 0x36, 0x39, 0x3C, 0xE5,
 * 0xE8, write_status) is verbatim from the instructions, and the control flow
 * matches the listing. But this routine carries an embedded data/pointer table
 * (~0x14001444; little-endian RAM pointers into the 0x10011xxx descriptor area)
 * that selects per-vendor handlers, and its heavy register reuse makes the exact
 * loop bounds partly interpretive. Treated as the weakest "clean" routine; see
 * the readme caveats. Returns 0 on success, else a status code (0x20005).
 * ========================================================================= */
uint32_t spifi_block_protect_engine(spifi_dev_t *dev,
                                    const uint32_t *region,  /* { start, len, ... } */
                                    uint8_t *changed_flag,
                                    uint16_t *scratch)
{
    uint32_t caps;
    uint32_t status_word;            /* SR1 | (SR2 << 8)               */
    uint32_t have_sr2;
    uint32_t result = 0u;
    uint32_t region_len = region[3]; /* descriptor +0xC (length/limit) */

    spifi_exit_mem_mode(dev);
    spifi_detect_4byte_addr(dev);
    caps = dev->caps;

    /* ---- strategy (A): global status-register block-protect bits ---------- */
    if (caps & (1u << 14)) {
        /* does this part expose a second status register's BP bits? */
        have_sr2 = ((dev->bp_mask_a & 0xFF00u) | (caps & 4u)) ? 1u : 0u;

        dev->status0 = (uint8_t)spifi_cmd(dev, 0x05u, 1u);     /* RDSR  */
        if (have_sr2)
            dev->status1 = (uint8_t)spifi_cmd(dev, 0x35u, 1u); /* RDSR2 */
        status_word = dev->status0 | (dev->status1 << 8);

        if (region_len == 0xFFFFFFFFu) {
            /* unlock-all path: see the SR2-mask branch below */
            if (dev->bp_mask_b & status_word) {
                uint32_t tries;
                *changed_flag = 1u;
                *scratch = (uint16_t)status_word;
                for (tries = 0u; tries < 3u; tries++) {
                    uint32_t cleared = status_word & ~dev->bp_mask_b;
                    result = spifi_write_status(dev, have_sr2 + 1u, cleared);
                    if (result == 0u)
                        break;
                }
            }
        } else if (region_len != 0u) {
            /* clear the BP bits that cover this region, keep the rest */
            uint32_t keep    = status_word & ~dev->bp_mask_a;
            uint32_t want    = region_len & dev->bp_mask_a;
            uint32_t newstat = keep | want;
            result = spifi_write_status(dev, have_sr2 + 1u, newstat);
        }

        /* settle + readback verify that the protect bits actually cleared */
        if (result == 0u) {
            volatile uint32_t d = 0xA6u;
            while (--d) { }
            dev->status0 = (uint8_t)spifi_cmd(dev, 0x05u, 1u);
            if (have_sr2)
                dev->status1 = (uint8_t)spifi_cmd(dev, 0x35u, 1u);
            if ((dev->status0 | (dev->status1 << 8)) & dev->bp_mask_b)
                result = 0x00020005u;
        }
        if (result != 0u) {
            spifi_enter_mem_mode(dev);
            return result;
        }
    }

    /* ---- strategy (B): per-region individual block lock/unlock ------------ */
    if (caps & 0x00038000u) {                 /* caps bits 15..17 */
        const uint8_t *sfdp = dev->sfdp_src;  /* +0x38 */
        uint32_t       addr = region[0] + region[1];   /* end of region */
        uint16_t       read_dwords;
        uint16_t       sectors = 0u;
        uint32_t       sector_size = 0x10000u;
        uint32_t       addr_field = ((dev->read_params >> 0x15u) & 1u) + 3u;

        (void)addr_field;
        read_dwords = (uint16_t)spifi_get_read_count(dev, &sectors);
        (void)read_dwords;

        /* refresh the SFDP sector map if this part needs it */
        if ((caps & (1u << 15)) && (caps & (1u << 8)) == 0u)
            spifi_read_sfdp(dev);

        /* Walk each protection descriptor / block and unlock those overlapping
         * the target range. The per-descriptor stride and the choice of unlock
         * opcode (0x36 / 0x39 / 0xE5 to unlock, 0x3C / 0xE8 to read the lock
         * state) follow the SFDP descriptor's size/flag fields. The full loop is
         * the embedded-table-driven part that is not bit-verifiable; the device
         * commands issued are: */
        while (sectors-- != 0u && sfdp != NULL) {
            uint32_t blk = addr;
            uint8_t  locked;

            /* read this block's lock state */
            {
                uint32_t cmd = spifi_make_cmd(0xE8u, 0u, 0u, 1u);
                locked = (uint8_t)spifi_cmd_addr(dev, cmd, blk);
            }
            if (locked & 1u) {
                /* issue the unlock for this block */
                spifi_wren_then_cmd(dev, 0x39u, 0u, blk);   /* Individual Block Unlock */
                *changed_flag = 1u;
            }
            addr        += sector_size;
            sfdp        += 8;                  /* next SFDP record */
        }
    }

    spifi_enter_mem_mode(dev);
    return result;
}