/* crypto_stream.c — 80K_4320_Bootloader (reconstructed)
 *
 * Software xorshift128 (Marsaglia) keystream cipher. This is NOT authenticated
 * encryption: a 128-bit-keyed PRNG, a positional XOR keystream, and a 16-bit
 * additive checksum. Keys live in keys.c; the per-word I/O dispatch (mem_read)
 * lives in flash_if.c. Names are kept identical to the disassembly.
 *
 * Shared rules across decrypt + checksum:
 *  - The cipher is POSITIONAL: keystream word N is consumed for image word N,
 *    so the PRNG is advanced for every word even when the word is not XORed.
 *  - Image words 128..143 (byte offsets 0x200..0x23F, the 64-byte cleartext
 *    header) are passed through VERBATIM so the loader can read the magic and
 *    length before it knows the key.
 *  - When the source is XIP flash (top byte 0x14), each <=16-word chunk is
 *    staged into a RAM scratch buffer via mem_read before processing; a RAM
 *    source is consumed in place. This is an I/O detail; the result is identical.
 */
#include "bootloader.h"

/* True when an address lies in the memory-mapped SPIFI window (0x14xxxxxx). */
#define IS_XIP(p)   ((((uintptr_t)(p)) & 0xFF000000u) == SPIFI_XIP_BASE)

/* ----------------------------------------------------------------------- */
/* PRNG core                                                               */
/* ----------------------------------------------------------------------- */

/* Textbook 32-bit xorshift128, shift triple (11, 19, 8). State = [x,y,z,w].
 * Returns one keystream word and advances the state. */
uint32_t xorshift128_next(uint32_t *state)
{
    uint32_t t = state[0] ^ (state[0] << 11);
    uint32_t w;

    state[0] = state[1];
    state[1] = state[2];
    state[2] = state[3];
    w        = state[3];

    state[3] = (w ^ (w >> 19)) ^ t ^ (t >> 8);
    return state[3];
}

/* Seed the state from a 16-byte key (k0..k3, little-endian dwords): the words
 * are whitened with K = 0x13579BDF and rotated by one ->  [k1,k2,k3,k0] ^ K. */
uint32_t *prng_seed_from_key(uint32_t *state, const uint32_t *key)
{
    state[0] = key[1] ^ PRNG_WHITEN_MASK;   /* x */
    state[1] = key[2] ^ PRNG_WHITEN_MASK;   /* y */
    state[2] = key[3] ^ PRNG_WHITEN_MASK;   /* z */
    state[3] = key[0] ^ PRNG_WHITEN_MASK;   /* w */
    return state;
}

/* Seed from the embedded Key A (flash 0x14002F70 / RAM 0x10016B94). */
uint32_t *prng_seed_keyA(uint32_t *state)
{
    return prng_seed_from_key(state, (const uint32_t *)g_keyA);
}

/* Seed from the optional per-unit key at 0x14000218 if it is provisioned, else
 * from the embedded Key B. A slot that is all-0x00 OR all-0xFF counts as blank.
 * (dwords_all_eq returns 0 when every dword matches the value — see flash_if.c.)
 * In this image the slot is all-0xFF, so Key B is used. */
uint32_t *prng_seed_keyB_or_device(uint32_t *state)
{
    uint8_t slot[16];

    mem_read(slot, (const void *)DEVICE_KEY_SLOT_ADDR, 16);

    if (dwords_all_eq(slot, 0x00000000u, 16) == 0 ||   /* all zero  -> blank */
        dwords_all_eq(slot, 0xFFFFFFFFu, 16) == 0) {   /* all 0xFF  -> blank */
        return prng_seed_from_key(state, (const uint32_t *)g_keyB);
    }
    return prng_seed_from_key(state, (const uint32_t *)slot);
}

/* ----------------------------------------------------------------------- */
/* Decrypt / verify                                                        */
/* ----------------------------------------------------------------------- */

/* Decrypt (or copy) a whole image into dst. Every ciphertext word is XORed with
 * one keystream word, except the 16-word header window [128..143] which is
 * copied through unchanged. Returns 1 on success, 0 on a bad argument. */
int stream_decrypt_skip_header(void *dst, const uint32_t *src,
                               uint32_t byte_len, uint32_t *state)
{
    uint32_t  scratch[16];                 /* <=16-word (64-byte) staging buffer */
    uint32_t *out = (uint32_t *)dst;
    uint32_t  total_words, idx;

    if (byte_len == 0)        return 1;     /* nothing to do                     */
    if (src == NULL)          return 0;
    if (dst == NULL)          return 0;
    if (byte_len & 3u)        return 0;     /* length must be word-aligned        */

    total_words = byte_len >> 2;

    for (idx = 0; idx < total_words; ) {
        uint32_t        chunk = total_words - idx;
        const uint32_t *in;

        if (chunk > 16u) chunk = 16u;

        if (IS_XIP(src)) {                  /* stage flash -> RAM, then process   */
            mem_read(scratch, src, chunk * 4u);
            in = scratch;
        } else {
            in = src;                       /* RAM source: in place               */
        }
        src += chunk;

        for (uint32_t k = 0; k < chunk; k++, idx++) {
            uint32_t ks = xorshift128_next(state);     /* advance EVERY word      */
            uint32_t w  = in[k];

            if (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                out[idx] = w;               /* header window: verbatim            */
            else
                out[idx] = w ^ ks;          /* body: XOR keystream                */
        }
    }
    return 1;
}

/* The per-key acceptance test: decrypt on the fly (same header-window rule) and
 * sum the plaintext words. The image is valid only if the FULL 32-bit
 * accumulator equals 0x0000FFFF.
 *
 * NB the name says "16" but the comparison is 32-bit, not "sum mod 2^16": an
 * image whose low 16 bits happen to be 0xFFFF still fails unless the high 16
 * bits are zero too. (Key B yields 0x0000FFFF here; Key A yields 0x9941791F.) */
int stream_checksum16(const uint32_t *src, uint32_t byte_len, uint32_t *state)
{
    uint32_t scratch[16];
    uint32_t total_words, idx, sum = 0;

    if (byte_len == 0) return 1;
    if (src == NULL)   return 0;

    total_words = byte_len >> 2;

    for (idx = 0; idx < total_words; ) {
        uint32_t        chunk = total_words - idx;
        const uint32_t *in;

        if (chunk > 16u) chunk = 16u;

        if (IS_XIP(src)) {
            mem_read(scratch, src, chunk * 4u);
            in = scratch;
        } else {
            in = src;
        }
        src += chunk;

        for (uint32_t k = 0; k < chunk; k++, idx++) {
            uint32_t ks    = xorshift128_next(state);
            uint32_t w     = in[k];
            uint32_t plain = (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                             ? w : (w ^ ks);
            sum += plain;
        }
    }
    return (sum == 0x0000FFFFu) ? 1 : 0;
}

/* Positional segment decrypt (segmented boot path). The PRNG is reseeded by the
 * caller, then fast-forwarded here by image_offset/4 words so the keystream
 * lines up with this segment's absolute position. The verbatim window is the
 * 16 words [skip_words .. skip_words+15] in absolute image coordinates
 * (skip_words is 0x80 in practice -> the same 0x200..0x23F header window).
 * Returns 1 on success, 0 on a bad argument. */
int stream_decrypt_segment(void *dst, uint32_t image_base, uint32_t image_offset,
                           uint32_t byte_len, uint32_t skip_words, uint32_t *state)
{
    uint32_t        scratch[16];
    uint32_t       *out = (uint32_t *)dst;
    const uint32_t *seg_src;
    uint32_t        idx, end_word;

    if (byte_len == 0)                  return 1;
    if (image_base == 0)                return 0;
    if (dst == NULL)                    return 0;
    if ((byte_len | image_offset) & 3u) return 0;   /* both word-aligned          */

    idx      = image_offset >> 2;                   /* absolute start word        */
    end_word = (image_offset + byte_len) >> 2;
    seg_src  = (const uint32_t *)(image_base + image_offset);

    /* fast-forward the keystream to the segment's absolute word offset */
    for (uint32_t i = 0; i < (image_offset >> 2); i++)
        (void)xorshift128_next(state);

    for (; idx < end_word; ) {
        uint32_t        chunk = end_word - idx;
        const uint32_t *in;

        if (chunk > 16u) chunk = 16u;

        if (IS_XIP(seg_src)) {
            mem_read(scratch, seg_src, chunk * 4u);
            in = scratch;
        } else {
            in = seg_src;
        }
        seg_src += chunk;

        for (uint32_t k = 0; k < chunk; k++, idx++, out++) {
            uint32_t ks = xorshift128_next(state);
            uint32_t w  = in[k];

            if (idx >= skip_words && (idx - skip_words) <= 15u)
                *out = w;                   /* verbatim window                    */
            else
                *out = w ^ ks;
        }
    }
    return 1;
}