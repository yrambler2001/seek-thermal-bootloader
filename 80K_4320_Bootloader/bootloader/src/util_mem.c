/* util_mem.c — 80K_4320_Bootloader (reconstructed)
 *
 * Small copy/compare/fill primitives. memcpy_words/halfwords/auto + memset_bytes
 * + memcmp_bytes are the developer's own helpers (they mirror the __aeabi copy
 * idioms). memcpy_fast is the toolchain's optimized memcpy pulled in by the
 * linker (a hand-unrolled 64-byte inner loop) and labelled by IDA — we keep the
 * name but defer to the C library rather than re-hand-rolling it. Names are kept
 * identical to the disassembly.
 *
 * The odd return values (e.g. &last-word-written) are preserved from the asm;
 * callers ignore them.
 */

#include "bootloader.h"
#include <string.h>   /* memcpy (the routine IDA labelled memcpy_fast) */

/* Word copy. Pre-increment store (matches STR.W Rx,[Rd,#4]!). */
void *memcpy_words(void *dst, const void *src, int len)
{
    uint32_t       *d   = (uint32_t *)dst - 1;
    const uint32_t *s   = (const uint32_t *)src;
    const uint8_t  *end = (const uint8_t *)src + len;
    while ((const uint8_t *)s != end)
        *++d = *s++;
    return d;                                  /* &last word written (as in the asm) */
}

/* Halfword copy (asm uses LDRSH but stores only the 16 bits, so unsigned is fine). */
void *memcpy_halfwords(void *dst, const void *src, int len)
{
    uint16_t       *d   = (uint16_t *)dst - 1;
    const uint16_t *s   = (const uint16_t *)src;
    const uint8_t  *end = (const uint8_t *)src + len;
    while ((const uint8_t *)s != end)
        *++d = *s++;
    return d;
}

/* Alignment dispatcher: word copy if dst|src|len are all 4-aligned, else
 * halfword copy if 2-aligned, else a byte loop. (This is what mem_read uses.) */
void *memcpy_auto(void *dst, const void *src, int len)
{
    uint32_t mix = (uint32_t)(uintptr_t)dst | (uint32_t)(uintptr_t)src | (uint32_t)len;
    if ((mix & 3u) == 0u)
        return memcpy_words(dst, src, len);
    if ((mix & 1u) == 0u)
        return memcpy_halfwords(dst, src, len);
    {
        uint8_t       *d   = (uint8_t *)dst - 1;
        const uint8_t *s   = (const uint8_t *)src;
        const uint8_t *end = s + len;
        while (s != end)
            *++d = *s++;
        return d;
    }
}

void *memset_bytes(void *dst, int val, int len)
{
    uint8_t *p   = (uint8_t *)dst;
    uint8_t *end = p + len;
    while (p != end)
        *p++ = (uint8_t)val;
    return p;                                  /* one-past-end (as in the asm) */
}

/* Byte compare; returns a[i]-b[i] at the first difference, else 0. */
int memcmp_bytes(const void *a, const void *b, int len)
{
    const uint8_t *pa  = (const uint8_t *)a;
    const uint8_t *pb  = (const uint8_t *)b;
    const uint8_t *end = pb + len;
    while (pb != end) {
        int diff = (int)*pa - (int)*pb;
        if (diff)
            return diff;
        ++pa;
        ++pb;
    }
    return 0;
}

/* IDA: memcpy_fast — the linker-supplied optimized memcpy (64-byte unrolled
 * inner loop + alignment fast-paths). Used by mem_read_any for RAM->RAM copies.
 * Behaviourally identical to the C library memcpy, so we defer to it. */
void *memcpy_fast(void *dst, const void *src, uint32_t len)
{
    return memcpy(dst, src, len);
}