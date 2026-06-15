/* util_mem.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * The small copy primitives the boot logic owns: a word copy with a byte tail,
 * and a plain byte copy. Names are kept identical to the disassembly. Callers
 * ignore the return values; dst is returned for convenience.
 *
 * NOTE: this image has only these two helpers (the iOS image also had
 * memcpy_halfwords and memset_bytes). Two further copy routines —
 * memcpy_fast (0x14000BE8) and memcpy_bytes_thunk (0x14002948) — live INSIDE the
 * imported SPIFI driver blob and are NOT reproduced here. memcpy_bytes_thunk is
 * the cross-boundary case noted in spifi_glue.c: after the blob relocates to
 * SRAM it branches back to memcpy_bytes below (flash 0x14000844), a RAM->flash
 * call.
 */
#include "bootloader.h"

/* Word copy with a trailing byte fixup: copy as many whole 32-bit words as fit,
 * then finish any 1..3 leftover bytes. This is the workhorse used by mem_read-
 * style copies throughout the boot logic (header reads, staging, the handoff
 * vector-table install). */
void *memcpy_auto(void *dst, const void *src, uint32_t len)
{
    uint8_t       *d     = (uint8_t *)dst;
    const uint8_t *s     = (const uint8_t *)src;
    uint32_t       words = len >> 2;

    while (words--) {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
    }
    for (len &= 3u; len; len--)
        *d++ = *s++;

    return dst;
}

/* Plain byte copy. Also the branch target of the driver's relocated
 * memcpy_bytes_thunk (see file header / spifi_glue.c). */
void *memcpy_bytes(void *dst, const void *src, uint32_t len)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (len--)
        *d++ = *s++;

    return dst;
}