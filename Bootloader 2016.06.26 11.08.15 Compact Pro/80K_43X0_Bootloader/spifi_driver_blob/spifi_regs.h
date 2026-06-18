// spifi_driver_blob/spifi_regs.h
/* spifi_regs.h — reconstructed interface to the bespoke SPIFI driver blob.
 *
 * This header is a RECONSTRUCTION aid, not part of the original sources: the
 * driver ships as a relocated binary blob (flash 0x14000BE8 -> RAM 0x10010000,
 * 0x1D70 bytes) and carries no symbols. The SPIFI controller register layout is
 * the standard LPC43xx block; the device-descriptor struct below is INFERRED
 * from the blob's field accesses (offsets are confirmed against the
 * disassembly; several field *meanings* past the obvious ones are best-effort).
 *
 * The descriptor instance is the bootloader's driver context at 0x10011EE4
 * (g_spifi_driver_ctx[0x80] in spifi_glue.c). The init/probe entry fills it from
 * the JEDEC ID + SFDP; every other routine reads it.
 */
#ifndef SPIFI_REGS_H
#define SPIFI_REGS_H

#include <stdint.h>
#include <stddef.h>

/* ---- SPIFI controller register block (LPC43xx, base 0x40003000) ---------- */
typedef volatile struct {
    uint32_t CTRL;     /* 0x00 control                                        */
    uint32_t CMD;      /* 0x04 command (opcode/frame/field/intlen/datalen)    */
    uint32_t ADDR;     /* 0x08 address                                        */
    uint32_t IDATA;    /* 0x0C intermediate data                              */
    uint32_t CLIMIT;   /* 0x10 cache limit                                    */
    uint32_t DATA;     /* 0x14 data FIFO                                      */
    uint32_t MCMD;     /* 0x18 memory command (enters XIP)                    */
    uint32_t STAT;     /* 0x1C status (bit1 CMD active, bit4 RESET, bit5 INTRQ)*/
} spifi_regs_t;

#define SPIFI   ((spifi_regs_t *)0x40003000u)

/* STAT bits actually tested by the blob */
#define SPIFI_STAT_CMD      0x00000002u   /* command in progress             */
#define SPIFI_STAT_RESET    0x00000010u
#define SPIFI_STAT_MCINIT   0x00000001u

/* CMD field helpers (the blob extracts the low 14 bits as a data byte count) */
#define SPIFI_CMD_DLEN_MASK 0x00003FFFu
#define SPIFI_CMD_DOUT      0x00008000u   /* data-out direction              */

/* ---- inferred device descriptor (the 0x80-byte driver context) ----------- *
 * Offsets are confirmed from the disassembly's field accesses; names past the
 * JEDEC/quad/erase fields are best-effort. Padding keeps every named field at
 * its observed offset. */
typedef struct {
    /* 0x00 */ uint32_t rsvd00;
    /* 0x04 */ uint32_t rsvd04;
    /* 0x08 */ uint32_t size_field;       /* set_capacity stores 1<<(n+1) here */
    /* 0x0C */ uint32_t capacity;         /* device size in bytes (cap 0x8000000) */
    /* 0x10 */ uint8_t  mfgr_id;          /* JEDEC manufacturer byte           */
    /* 0x11 */ uint8_t  dev_type;         /* JEDEC memory-type byte            */
    /* 0x12 */ uint8_t  dev_capacity;     /* JEDEC capacity byte               */
    /* 0x13 */ uint8_t  wip_bit;          /* status WIP bit position           */
    /* 0x14 */ uint8_t  status0;          /* scratch: status register          */
    /* 0x15 */ uint8_t  status1;          /* scratch: status-2 register        */
    /* 0x16 */ uint16_t rsvd16;
    /* 0x18 */ uint16_t bp_mask_a;        /* block-protect mask (status)       */
    /* 0x1A */ uint16_t bp_mask_b;        /* block-protect mask (status-2)     */
    /* 0x1C */ uint32_t read_params;      /* read mode / dummy capability bits */
    /* 0x20 */ uint32_t cmd_base;         /* program/erase opcode + field bits */
    /* 0x24 */ uint16_t sfdp_read_count;  /* bytes to read from SFDP           */
    /* 0x26 */ uint16_t sfdp_param_dwords;/* parameter dwords summed from SFDP */
    /* 0x28 */ uint32_t caps;             /* feature flags (bit18 = quad)      */
    /* 0x2C */ uint32_t status_poll;      /* busy/WIP poll configuration       */
    /* 0x30 */ uint8_t  size_shift;       /* page/sector size shift            */
    /* 0x31 */ uint8_t  rsvd31;
    /* 0x32 */ uint8_t  dummy_cycles;     /* read dummy-cycle count            */
    /* 0x33 */ uint8_t  rsvd33;
    /* 0x34 */ uint8_t  erase_opcode;     /* sector/block erase opcode         */
    /* 0x35 */ uint8_t  rsvd35[3];
    /* 0x38 */ const uint8_t *sfdp_src;   /* SFDP parameter source pointer     */
    /* 0x3C */ uint8_t  sfdp_buf[0x44];   /* SFDP parameter buffer (..0x80)    */
} spifi_dev_t;

/* ---- command primitives (spifi_cmd.c) ------------------------------------ */
unsigned int spifi_make_cmd(uint32_t opcode, uint32_t frame,
                            uint32_t intlen, uint32_t datalen);
uint32_t     spifi_quad_opt_bits(spifi_dev_t *dev);
uint32_t     spifi_cmd_read(uint32_t cmd);
uint32_t     spifi_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t datalen);
uint32_t     spifi_cmd_addr(spifi_dev_t *dev, uint32_t cmd, uint32_t addr);
void         spifi_cmd_data(spifi_dev_t *dev, uint32_t opcode, uint32_t frame,
                            uint32_t addr);
uint32_t     spifi_write_enable(spifi_dev_t *dev);
void         spifi_wren_then_cmd(spifi_dev_t *dev, uint32_t opcode, uint32_t frame,
                                 uint32_t addr);
void         spifi_cmd_addr_data(spifi_dev_t *dev, uint32_t cmd, uint32_t addr,
                                 uint32_t data);
void        *memcpy_fast(void *dst, const void *src, unsigned int len);

#endif /* SPIFI_REGS_H */