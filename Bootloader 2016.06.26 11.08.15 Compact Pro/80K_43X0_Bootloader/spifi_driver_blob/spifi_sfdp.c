// spifi_driver_blob/spifi_sfdp.c
/* spifi_sfdp.c — reconstructed from the driver blob disassembly (clean region).
 *
 * SFDP (JEDEC JESD216 Serial Flash Discoverable Parameters) handling plus two
 * device-geometry helpers. The blob uses SFDP to size the part and pick a read
 * count when the JEDEC ID alone is not enough; the vendor-config paths
 * (spifi_vendor.c) call these.
 *
 * All five bodies lifted cleanly. Descriptor (spifi_dev_t) offsets are confirmed
 * against the instructions; the field names are the best-effort interpretation
 * from spifi_regs.h. The SFDP dword layout (param-table stride 8, the
 * count/dwords fields at +0x24/+0x26) is read verbatim from the accesses; the
 * JESD216 *meaning* of those fields is annotated for the reader.
 */
#include "spifi_regs.h"

extern uint32_t spifi_quad_opt_bits(spifi_dev_t *dev);
extern uint32_t spifi_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t datalen);

/* ===========================================================================
 * spifi_detect_4byte_addr  (flash 0x14000F2A)
 *
 * If the device advertises 4-byte addressing capability (caps bit 7), read the
 * flag-status / config byte (0x2B), stash it, and update the read_params /
 * caps with either the 4-byte (0x10000) or 3-byte (0x4000) address-field width.
 * Verbatim opcode 0x2B and field masks.
 * ========================================================================= */
uint32_t spifi_detect_4byte_addr(spifi_dev_t *dev)
{
    if ((dev->caps & 0x00000080u) == 0u)                 /* bit7: 4B capable? */
        return (uint32_t)(uintptr_t)dev;

    {
        uint32_t v = (uint8_t)spifi_cmd(dev, 0x2Bu, 1u);
        uint32_t caps = dev->caps & ~0x0003C000u;        /* clear addr-width field */
        dev->status1 = (uint8_t)v;
        if (v & 0x00000080u)                             /* bit7 set -> 4-byte */
            caps |= 0x00010000u;
        else
            caps |= 0x00004000u;
        dev->caps = caps;
    }
    return (uint32_t)(uintptr_t)dev;
}

/* ===========================================================================
 * spifi_sum_sfdp_params  (flash 0x14000F5C)
 *
 * Walk the SFDP parameter table at 'src' (length 'len' bytes, 8-byte records)
 * and accumulate two running totals into the descriptor:
 *   +0x24 sfdp_read_count   += record halfword at +6
 *   +0x26 sfdp_param_dwords += (record halfword at +6) * (record byte at +4 ?
 *                              2 : 1) + carry
 * then finally fold +0x26 down to a 13-bit count (UBFX #3,#0xD).
 *
 * This is the densest clean routine in the file; the arithmetic is faithful to
 * the MLA/ADD sequence. The JESD216 reading: each record describes a region's
 * sector count and size class, and the blob is summing total addressable dwords.
 * ========================================================================= */
uint32_t spifi_sum_sfdp_params(spifi_dev_t *dev, const uint8_t *src, uint32_t len)
{
    const uint8_t *p = src;
    uint32_t acc = 7u;                  /* +0x26 seed (MOVS #7) */

    dev->sfdp_src         = src;
    dev->sfdp_read_count  = 0u;
    dev->sfdp_param_dwords = 7u;

    while (len >= 8u) {
        uint16_t rec_hw   = (uint16_t)(p[6] | (p[7] << 8));
        uint8_t  rec_flag = p[4];
        uint32_t mult     = (rec_flag & 1u) ? 2u : 1u;

        dev->sfdp_read_count = (uint16_t)(dev->sfdp_read_count + rec_hw);
        acc = (uint32_t)rec_hw * mult + acc;
        dev->sfdp_param_dwords = (uint16_t)acc;

        p   += 8;
        len -= 8;
    }
    dev->sfdp_param_dwords = (uint16_t)((acc >> 3) & 0x1FFFu);
    return (uint32_t)(uintptr_t)dev;
}

/* ===========================================================================
 * spifi_get_read_count  (flash 0x14000F98)
 *
 * Return the SFDP-derived read count, writing the byte count through *out if
 * non-NULL. If no SFDP source was recorded (+0x38 == 0), fall back to fields
 * packed in the descriptor's size word (+0x08): the halfword at >>16 and the
 * count at >>19. Faithful to both branches.
 * ========================================================================= */
uint32_t spifi_get_read_count(spifi_dev_t *dev, uint16_t *out)
{
    if (dev->sfdp_src != NULL) {
        if (out != NULL)
            *out = dev->sfdp_read_count;
        return dev->sfdp_param_dwords;
    }
    /* no SFDP: derive from the size field */
    if (out != NULL)
        *out = (uint16_t)(dev->size_field >> 16);
    return dev->size_field >> 19;
}

/* ===========================================================================
 * spifi_read_sfdp  (flash 0x14000FB4)
 *
 * Read the SFDP parameter block into the descriptor's sfdp_buf (+0x3C). Get the
 * read count; if zero, return. Otherwise set the SFDP-read capability bit
 * (caps |= 0x100), build the SFDP read command (0x72200000 base | quad-opt |
 * count), stream the bytes into the buffer, zero-pad the remainder up to +0x80,
 * and spin to completion. Verbatim 0x72200000 base.
 * ========================================================================= */
uint32_t spifi_read_sfdp(spifi_dev_t *dev)
{
    uint16_t count = 0u;
    uint32_t n = spifi_get_read_count(dev, &count);
    uint8_t *dst = dev->sfdp_buf;          /* descriptor +0x3C */

    n = count;                              /* the BNE in the disasm uses count */
    if (n == 0u)
        return (uint32_t)(uintptr_t)dev;

    dev->caps |= 0x00000100u;               /* mark SFDP read in progress */

    {
        uint32_t cmd = spifi_quad_opt_bits(dev) | (uint32_t)count;
        cmd |= 0x72200000u;
        SPIFI->CMD = cmd;

        while (n != 0u) {
            *dst++ = (uint8_t)SPIFI->DATA;
            n = (uint16_t)(n - 1u);
        }
        /* zero-pad the rest of the 0x44-byte buffer */
        while (dst < dev->sfdp_buf + 0x44u)
            *dst++ = 0u;

        while (SPIFI->STAT & SPIFI_STAT_CMD) { }
    }
    return (uint32_t)(uintptr_t)dev;
}

/* ===========================================================================
 * spifi_set_capacity  (flash 0x14001024)
 *
 * Given a size shift 'n', store 1<<(n+1) at +0x08 and the clamped capacity at
 * +0x0C (capped at 0x8000000 = 128 MB). Tiny and exact.
 * ========================================================================= */
uint32_t spifi_set_capacity(spifi_dev_t *dev, uint8_t n)
{
    uint32_t size = 1u << (uint32_t)(n + 1u);
    uint32_t cap  = 0x08000000u;

    dev->size_field = size;
    if (size <= cap)
        cap = size;
    dev->capacity = cap;
    return (uint32_t)(uintptr_t)dev;
}