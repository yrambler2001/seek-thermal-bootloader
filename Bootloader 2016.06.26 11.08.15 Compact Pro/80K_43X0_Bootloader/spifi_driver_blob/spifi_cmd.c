// spifi_driver_blob/spifi_cmd.c
/* spifi_cmd.c — reconstructed from the driver blob disassembly (clean region).
 *
 * The low-level SPIFI command primitives: build a CMD word, OR in the quad
 * field-form bits, issue it, and read or write the small data phase. Every
 * higher-level routine (SFDP read, mode config, program/erase) is built on
 * these. Names and addresses are from the manual-label table; the bodies match
 * the lifted instructions.
 *
 * The CMD-word data-length field is the low 14 bits; the blob only ever issues
 * 0..3 byte phases plus a 4-byte "word" case, so the read/write helpers switch
 * on that count directly (faithful to the machine code).
 */
#include "spifi_regs.h"

/* ===========================================================================
 * memcpy_fast  (driver blob, flash 0x14000BE8)
 * Word copy when dst and src are both word-aligned, dropping to a byte tail;
 * otherwise a plain byte copy. NOTE the quirk reproduced verbatim: the aligned
 * loop runs only while more than 4 bytes remain, so the final word is always
 * finished by the byte tail. Returns dst.
 * ========================================================================= */
void *memcpy_fast(void *dst, const void *src, unsigned int len)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        while (len > 4u) {
            *(uint32_t *)d = *(const uint32_t *)s;
            d   += 4;
            s   += 4;
            len -= 4;
        }
    }
    while (len != 0u) {
        *d++ = *s++;
        len--;
    }
    return dst;
}

/* ===========================================================================
 * spifi_make_cmd  (flash 0x14000C5C)
 * Assemble a SPIFI CMD word: opcode<<24 | (0x200000 + frame<<21) |
 * intlen<<16 | datalen. The 0x200000 base biases the frame-form field.
 * ========================================================================= */
unsigned int spifi_make_cmd(uint32_t opcode, uint32_t frame,
                            uint32_t intlen, uint32_t datalen)
{
    uint32_t cmd = 0x00200000u + (frame << 21);
    cmd |= opcode  << 24;
    cmd |= intlen  << 16;
    cmd |= datalen;
    return cmd;
}

/* ===========================================================================
 * spifi_quad_opt_bits  (flash 0x14000C72)
 * If the device's quad-enable capability (caps bit 18) is set, return the
 * field-form bits 0x180000 to OR into a CMD word; otherwise 0.
 * ========================================================================= */
uint32_t spifi_quad_opt_bits(spifi_dev_t *dev)
{
    if (dev->caps & (1u << 18))
        return 0x00180000u;
    return 0u;
}

/* ===========================================================================
 * spifi_cmd_read  (flash 0x14000C2A)
 * Write a CMD word, then read its data phase (0/1/2/3 bytes, or a 4-byte word
 * for any larger count field) from the DATA FIFO, and spin until the command
 * completes (STAT bit 1 clears). Returns the assembled read value (0 if none).
 * ========================================================================= */
uint32_t spifi_cmd_read(uint32_t cmd)
{
    uint32_t r;

    SPIFI->CMD = cmd;
    switch (cmd & SPIFI_CMD_DLEN_MASK) {           /* low 14 bits = byte count */
    case 0u:  r = 0u; break;                        /* no data phase            */
    case 1u:  r = (uint8_t)SPIFI->DATA; break;
    case 2u:  r = (uint16_t)SPIFI->DATA; break;
    case 3u:  { uint32_t lo = (uint16_t)SPIFI->DATA;
                uint32_t hi = (uint8_t)SPIFI->DATA;
                r = lo | (hi << 16); } break;
    default:  r = SPIFI->DATA; break;               /* 4-byte word              */
    }
    while (SPIFI->STAT & SPIFI_STAT_CMD) { }
    return r;
}

/* ===========================================================================
 * spifi_cmd  (flash 0x14000C82)
 * Issue a single opcode with a data-read phase of datalen bytes. No address.
 * ========================================================================= */
uint32_t spifi_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t datalen)
{
    uint32_t cmd = spifi_make_cmd(opcode, 0u, 0u, datalen);
    cmd |= spifi_quad_opt_bits(dev);
    return spifi_cmd_read(cmd);
}

/* ===========================================================================
 * spifi_cmd_addr  (flash 0x14000CA4)
 * Issue a pre-built CMD word that carries an address phase. The caller supplies
 * the full CMD (opcode + frame + length); this only loads ADDR and ORs quad.
 * ========================================================================= */
uint32_t spifi_cmd_addr(spifi_dev_t *dev, uint32_t cmd, uint32_t addr)
{
    SPIFI->ADDR = addr;
    cmd |= spifi_quad_opt_bits(dev);
    return spifi_cmd_read(cmd);
}

/* ===========================================================================
 * spifi_cmd_data  (flash 0x14000CB6)
 * Issue opcode + address with no data read-back, then spin to completion.
 * (Used for mode-set commands that take an address but return nothing.)
 * ========================================================================= */
void spifi_cmd_data(spifi_dev_t *dev, uint32_t opcode, uint32_t frame,
                    uint32_t addr)
{
    uint32_t cmd;
    SPIFI->ADDR = addr;
    cmd  = spifi_make_cmd(opcode, frame, 0u, 0u);
    cmd |= spifi_quad_opt_bits(dev);
    SPIFI->CMD = cmd;
    while (SPIFI->STAT & SPIFI_STAT_CMD) { }
}

/* ===========================================================================
 * spifi_write_enable  (flash 0x14000CE2)
 * WREN (0x06).
 * ========================================================================= */
uint32_t spifi_write_enable(spifi_dev_t *dev)
{
    return spifi_cmd(dev, 0x06u, 0u);
}

/* ===========================================================================
 * spifi_wren_then_cmd  (flash 0x14000CE8)
 * WREN, then an opcode+address command (spifi_cmd_data).
 * ========================================================================= */
void spifi_wren_then_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t frame,
                         uint32_t addr)
{
    spifi_write_enable(dev);
    spifi_cmd_data(dev, opcode, frame, addr);
}

/* ===========================================================================
 * spifi_cmd_addr_data  (flash 0x14000D04)
 * WREN, load ADDR, issue a data-OUT command, and write its 0/1/2/3/4 data
 * bytes into the DATA FIFO, then spin to completion. The length is taken from
 * the supplied CMD word's low 14 bits.
 * ========================================================================= */
void spifi_cmd_addr_data(spifi_dev_t *dev, uint32_t cmd, uint32_t addr,
                         uint32_t data)
{
    uint32_t len = cmd & SPIFI_CMD_DLEN_MASK;

    spifi_write_enable(dev);
    SPIFI->ADDR = addr;
    SPIFI->CMD  = cmd | spifi_quad_opt_bits(dev) | SPIFI_CMD_DOUT;

    switch (len) {
    case 0u: break;
    case 1u: SPIFI->DATA = (uint8_t)data; break;
    case 2u: SPIFI->DATA = (uint16_t)data; break;
    case 3u: SPIFI->DATA = (uint16_t)data;          /* low 2 bytes */
             SPIFI->DATA = (uint8_t)(data >> 16);    /* 3rd byte    */
             break;
    default: SPIFI->DATA = data; break;              /* 4-byte word */
    }
    while (SPIFI->STAT & SPIFI_STAT_CMD) { }
}