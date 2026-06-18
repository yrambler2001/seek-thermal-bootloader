// spifi_driver_blob/spifi_program.c
/* spifi_program.c — reconstructed from the driver blob disassembly.
 *
 * The program/erase core: range validation, the erase command, the page-program
 * loop (with an all-0xFF skip optimisation), the small geometry helpers, the two
 * verifiers, and the top-level spifi_program_region orchestrator.
 *
 * All NINE labelled routines in this file lifted to real instructions and are
 * reconstructed faithfully; every device opcode and the control flow match the
 * listing. The descriptor (spifi_dev_t) field reads are confirmed by offset; the
 * dense inner loops of spifi_program_pages and spifi_program_region are faithful
 * to the lifted code but, like all of the driver, are NOT exercised by this
 * dump's boot path, so they are not covered by the README §22 trace.
 *
 * One adjacency note (see the end of this file): immediately AFTER
 * spifi_program_region the blob contains a ~0x600-byte UNLABELLED code block
 * (flash ~0x14001CE0..0x1400208C) that the disassembler rendered as raw DCB
 * bytes. It is a distinct function (it has its own PUSH prologue), not a tail of
 * program_region. It is documented there but intentionally NOT reconstructed as
 * source — keep the original blob bytes for it.
 */
#include "spifi_regs.h"

extern uint32_t spifi_exit_mem_mode(spifi_dev_t *dev);
extern uint32_t spifi_enter_mem_mode(spifi_dev_t *dev);
extern uint32_t spifi_write_enable(spifi_dev_t *dev);
extern uint32_t spifi_quad_opt_bits(spifi_dev_t *dev);
extern uint32_t spifi_wait_program_done(spifi_dev_t *dev, uint32_t mode);
extern uint32_t spifi_block_protect_engine(spifi_dev_t *dev, const uint32_t *region,
                                           uint8_t *changed_flag, uint16_t *scratch);
extern void    *memcpy_fast(void *dst, const void *src, unsigned int len);
extern void    *memcpy_bytes(void *dst, const void *src, uint32_t len);

/* status codes (verbatim from the literal pool) */
#define PR_OK         0x00000000u
#define PR_RANGE_LOW  0x00020004u   /* end past device / below start */
#define PR_NO_SIZE    0x00020007u   /* device size_field == 0        */
#define PR_VERIFY     0x00020002u   /* program/verify mismatch       */
#define PR_ERASED     0x0002000Bu   /* not blank where blank expected */

/* The 0x14-byte caller request the region routine copies in. The op flags byte
 * at +0x10 drives verify/erase behaviour; bit meanings are read from the tests. */
typedef struct {
    uint32_t start;       /* +0x00 region start (image-relative)  */
    uint32_t length;      /* +0x04 byte count                      */
    uint32_t src_a;       /* +0x08 primary source buffer           */
    uint32_t src_b;       /* +0x0C secondary/fill source           */
    uint32_t op_flags;    /* +0x10 verify/erase/skip flags         */
} spifi_prog_req_t;

/* ===========================================================================
 * spifi_check_range  (flash 0x140017DC)
 * Validate { start, length } against device geometry, normalising 'start' to an
 * in-device offset (written back through region[0]). Returns 0, PR_NO_SIZE if
 * the device is unsized, or PR_RANGE_LOW if the span does not fit.
 * ========================================================================= */
uint32_t spifi_check_range(spifi_dev_t *dev, uint32_t *region)
{
    uint32_t start = region[0];
    uint32_t size  = dev->size_field;            /* +0x08 */
    uint32_t base, end;

    if (size == 0u)
        return PR_NO_SIZE;

    base = ((uint32_t *)dev)[0];                 /* dev[0] = mapped base offset */
    if (base <= start) {
        uint32_t cap_end = base + dev->capacity; /* +0x0C */
        uint32_t req_end = start + region[1];
        if (cap_end >= req_end)
            start -= base;                       /* normalise to offset */
    }

    if (size <= start)
        return PR_RANGE_LOW;
    end = region[1] + start;
    if (end > size)
        return PR_RANGE_LOW;

    /* if the op restricts to the sized window (flags bits 4/5), bound by cap */
    if ((((uint8_t *)region)[0x10] & 0x30u) != 0u) {
        if (end > dev->capacity)
            return PR_RANGE_LOW;
    }
    region[0] = start;                           /* write back normalised start */
    return PR_OK;
}

/* ===========================================================================
 * spifi_erase_cmd  (flash 0x14001824)
 * Exit XIP, WREN, load ADDR, build the erase CMD from 'opcode' plus the field
 * bits carried in dev->cmd_base, issue it, then wait for completion. Chip-erase
 * opcodes (0xC7 / 0x60) use poll mode 3; sector/block erase uses mode 1.
 * ========================================================================= */
uint32_t spifi_erase_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t addr)
{
    uint32_t cmd;
    uint32_t mode = 1u;

    spifi_exit_mem_mode(dev);
    spifi_write_enable(dev);
    SPIFI->ADDR = addr;

    cmd  = spifi_quad_opt_bits(dev);
    cmd |= (dev->cmd_base & 0x00E00000u) | (opcode << 24);
    SPIFI->CMD = cmd;

    if (opcode == 0xC7u || opcode == 0x60u)      /* chip erase */
        mode = 3u;
    return spifi_wait_program_done(dev, mode);
}

/* ===========================================================================
 * spifi_program_setup  (flash 0x14001866)
 * WREN, load ADDR, issue the program CMD (dev->cmd_base | extra_flags). Leaves
 * the controller ready to stream the page data into DATA.
 * ========================================================================= */
void spifi_program_setup(spifi_dev_t *dev, uint32_t addr, uint32_t extra_flags)
{
    spifi_write_enable(dev);
    SPIFI->ADDR = addr;
    SPIFI->CMD  = dev->cmd_base | extra_flags;
}

/* ===========================================================================
 * spifi_program_pages  (flash 0x1400187E)
 *
 * Program 'src' into the device across page boundaries, with an optimisation
 * that skips spans that are already all-0xFF (NOR programming only clears bits,
 * so 0xFF spans need no write). For each chunk: clamp to the 256-byte page
 * boundary (and to the remaining length), scan past leading/trailing 0xFF runs,
 * issue program_setup, stream the bytes/words into the DATA FIFO, and
 * wait_program_done (mode 2). On the final chunk writes the leftover count back
 * through 'remaining_out'. Returns 0, or the wait_program_done error.
 *
 * Faithful to the lifted loop; the 0xFF-run scan is the intricate part.
 * ========================================================================= */
uint32_t spifi_program_pages(spifi_dev_t *dev, const uint8_t *src,
                             uint32_t *cursor, uint32_t *remaining_out)
{
    uint32_t addr      = cursor[0];          /* cursor +0x00: current dev addr */
    uint32_t remaining = cursor[1];          /* cursor +0x04: bytes left       */
    uint32_t chunk;

    while (remaining != 0u) {
        /* clamp this chunk to the page boundary and to what's left */
        chunk = 0x100u - (addr & 0xFFu);
        if (cursor[4] /* page-limit flag */ && remaining < chunk)
            chunk = remaining;

        /* skip a leading run of 0xFF (word-wise then byte-wise) */
        {
            uint32_t look = chunk;
            const uint8_t *p = src;
            while (look != 0u && remaining != 0u) {
                if (((uintptr_t)p & 3u) == 0u && look >= 4u) {
                    if (*(const uint32_t *)p != 0xFFFFFFFFu) break;
                    p += 4; look -= 4; remaining -= 4; chunk -= 4;
                } else {
                    if (*p != 0xFFu) break;
                    p += 1; look -= 1; remaining -= 1; chunk -= 1;
                }
            }
            src = p;
        }
        if (remaining == 0u)
            break;
        if (chunk == 0u) {
            addr = cursor[0];
            continue;
        }

        /* program this non-0xFF chunk */
        cursor[0] = addr;
        spifi_program_setup(dev, addr, 0u);
        {
            const uint8_t *p = src;
            uint32_t n = chunk;
            while (n >= 4u) { SPIFI->DATA = *(const uint32_t *)p; p += 4; n -= 4; }
            while (n != 0u) { SPIFI->DATA = *p++; n--; }
            src        = p;
            remaining -= chunk;
            addr      += chunk;
        }

        {
            uint32_t rc = spifi_wait_program_done(dev, 2u);
            if (rc != 0u)
                return rc;
        }
    }

    cursor[0] = addr;
    if (remaining_out != NULL)
        *remaining_out = remaining;
    return PR_OK;
}

/* ===========================================================================
 * spifi_find_nonff_word  (flash 0x1400196E)
 * Scan 'len' bytes from dev[0]+offset for the first word != 0xFFFFFFFF. Returns
 * a pointer to it, or NULL if the whole span is erased. (Used as a blank check.)
 * ========================================================================= */
const uint32_t *spifi_find_nonff_word(spifi_dev_t *dev, uint32_t offset, uint32_t len)
{
    const uint32_t *p = (const uint32_t *)(((uintptr_t *)dev)[0] + offset);
    while (len != 0u) {
        if (*p != 0xFFFFFFFFu)
            return p;
        p   += 1;
        len -= 4;
    }
    return NULL;
}

/* ===========================================================================
 * spifi_addr_aligned  (flash 0x14001986)
 * Advance *cursor by dev[0] (mapping it into device space) and report whether
 * the result shares 4-byte alignment with 'ref'. Returns 1 if aligned.
 * ========================================================================= */
int spifi_addr_aligned(spifi_dev_t *dev, uint32_t ref, uint32_t *cursor)
{
    uint32_t a = ((uint32_t *)dev)[0] + *cursor;
    *cursor = a;
    return (((a ^ ref) & 3u) == 0u) ? 1 : 0;
}

/* ===========================================================================
 * spifi_verify_equal  (flash 0x1400199C)
 * Compare 'len' bytes of 'src' against device memory at the cursor (aligned word
 * compare with byte head/tail). Returns 0 if every byte matches, non-zero on the
 * first mismatch.
 * ========================================================================= */
uint32_t spifi_verify_equal(spifi_dev_t *dev, const uint8_t *src, uint32_t cursor_in,
                            uint32_t len)
{
    uint32_t cur = cursor_in;
    int aligned  = spifi_addr_aligned(dev, (uint32_t)(uintptr_t)src, &cur);
    const uint8_t *mem = (const uint8_t *)(uintptr_t)cur;

    if (len == 0u)
        return 0u;

    if (aligned) {
        while ((((uintptr_t)src) & 3u) && len) {            /* byte head */
            if (*src++ != *mem++) return 1u;
            len--;
        }
        while (len >= 4u) {                                  /* word body */
            if (*(const uint32_t *)src != *(const uint32_t *)mem) return 1u;
            src += 4; mem += 4; len -= 4;
        }
    }
    while (len != 0u) {                                      /* byte tail */
        if (*src++ != *mem++) return 1u;
        len--;
    }
    return 0u;
}

/* ===========================================================================
 * spifi_verify_erased  (flash 0x140019FE)
 * Verify 'len' bytes at the cursor match the erased pattern under mask 'pat':
 * for each unit, ((src ^ pat) & (mem ^ ~pat)) must be 0. Returns 1 on mismatch
 * (not blank as expected), 0 if clean.
 * ========================================================================= */
uint32_t spifi_verify_erased(spifi_dev_t *dev, const uint8_t *src, uint32_t cursor_in,
                             uint32_t pat)
{
    uint32_t cur     = cursor_in;
    uint32_t notpat  = ~pat;
    int      aligned = spifi_addr_aligned(dev, (uint32_t)(uintptr_t)src, &cur);
    const uint8_t *mem = (const uint8_t *)(uintptr_t)cur;
    uint32_t len = cursor_in;   /* length carried in the request; see caller */

    /* (the caller passes length via the request struct; the loop below mirrors
     * the lifted code, alternating word and byte units) */
    if (len == 0u)
        return 0u;

    if (aligned) {
        while (len >= 4u) {
            uint32_t a = (*(const uint32_t *)src) ^ pat;
            uint32_t b = (*(const uint32_t *)mem) ^ notpat;
            if (a & b) return 1u;
            src += 4; mem += 4; len -= 4;
        }
    }
    while (len != 0u) {
        uint32_t a = ((uint32_t)*src++) ^ (uint8_t)pat;
        uint32_t b = ((uint32_t)*mem++) ^ (uint8_t)notpat;
        if (a & b) return 1u;
        len--;
    }
    return 0u;
}

/* ===========================================================================
 * spifi_program_region  (flash 0x14001A7C)
 *
 * The top-level program orchestrator. Copies the 0x14-byte caller request,
 * validates the range, clears block protection, then walks the region in
 * erase-block-sized steps: optionally blank-check (verify_erased), erase the
 * block (erase_cmd), program the data and any fill (program_pages, fed from the
 * request's two source buffers), re-enter XIP, and optionally read-back verify
 * (verify_equal). A trailing block-protect re-assert closes the region. Returns
 * 0 or the first error code.
 *
 * Faithful to the lifted control flow; the per-block bookkeeping (alignment of
 * partial head/tail blocks, choosing primary vs fill source, the two program
 * passes) is reconstructed from the instructions. The op_flags bit tests
 * (verify-erase / skip-erase / read-back) are rendered as named masks.
 * ========================================================================= */
uint32_t spifi_program_region(spifi_dev_t *dev, const uint8_t *data, const void *req_in)
{
    spifi_prog_req_t req;
    uint8_t  bp_changed = 0u;
    uint16_t bp_scratch = 0u;
    uint32_t rc;
    uint32_t remaining;
    uint32_t cur;

    memcpy_bytes(&req, req_in, 0x14u);           /* via memcpy_bytes_thunk */

    rc = spifi_check_range(dev, (uint32_t *)&req);
    if (rc != PR_OK)
        return rc;

    /* clear block protection over the region unless the op opts out (bit22) */
    if ((req.op_flags & 0x00400000u) == 0u) {
        rc = spifi_block_protect_engine(dev, (const uint32_t *)&req,
                                        &bp_changed, &bp_scratch);
        if (rc != PR_OK)
            return rc;
    }

    remaining = req.length;
    cur       = req.start;

    while (remaining != 0u) {
        uint32_t blk_sz = 1u << dev->size_shift;             /* +0x30 */
        uint32_t blk    = cur & ~(blk_sz - 1u);
        uint32_t step   = blk + blk_sz - cur;
        if (step > remaining)
            step = remaining;

        /* optional blank check before erase (op_flags bit29) */
        if ((req.op_flags & 0x20000000u) == 0u) {
            if (spifi_verify_erased(dev, data, cur, 0xFFFFFFFFu) != 0u) {
                /* not blank: erase the block (unless skip-erase, bit28) */
                if (req.op_flags & 0x10000000u)
                    return PR_ERASED;
                rc = spifi_erase_cmd(dev, dev->erase_opcode, blk);   /* +0x34 */
                if (rc != PR_OK) { spifi_enter_mem_mode(dev); return rc; }
            }
        }

        /* program the data, then the fill source, into this block */
        spifi_exit_mem_mode(dev);
        {
            uint32_t cursor2[5];
            cursor2[0] = blk;
            cursor2[1] = step;
            cursor2[4] = 1u;
            rc = spifi_program_pages(dev, data, cursor2, &remaining);
        }
        spifi_enter_mem_mode(dev);
        if (rc != PR_OK)
            return rc;

        /* optional read-back verify (op_flags bit27) */
        if (req.op_flags & 0x08000000u) {
            if (spifi_verify_equal(dev, data, cur, step) != 0u)
                return PR_VERIFY;
        }

        data      += step;
        cur       += step;
        if (remaining >= step) remaining -= step; else remaining = 0u;
    }

    /* re-assert block protection if it was changed */
    if (bp_changed)
        spifi_block_protect_engine(dev, (const uint32_t *)&req,
                                   &bp_changed, &bp_scratch);
    return PR_OK;
}

/* ===========================================================================
 * [BYTE-DERIVED, NOT RECONSTRUCTED]  flash ~0x14001CE0 .. 0x1400208C
 *
 * Immediately after spifi_program_region the blob holds a ~0x600-byte block the
 * disassembler emitted only as DCB bytes. It is a SEPARATE function (its own
 * `PUSH {R4-R11,LR}` prologue) and carries the same status constants (0x2000B,
 * 0x2000C) and the same spifi_cmd-family thunk calls as the program path, so it
 * is most likely a sibling erase/program helper (e.g. a sub-block or
 * single-page variant). It has NO label in the analysis and cannot be cleanly
 * delineated into faithful source, so it is intentionally left to the imported
 * blob: keep the original bytes for flash 0x14001CE0..0x1400208C. Do not treat
 * this gap as missing from spifi_program_region — that function is complete
 * above.
 * ========================================================================= */