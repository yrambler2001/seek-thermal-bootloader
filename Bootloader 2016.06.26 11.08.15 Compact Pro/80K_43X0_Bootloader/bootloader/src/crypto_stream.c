/* crypto_stream.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Software xorshift128 (Marsaglia) keystream cipher, shift triple 11/19/8. This
 * is NOT authenticated encryption: a 128-bit-keyed PRNG, a positional XOR
 * keystream, and a 32-bit additive checksum. Keys live in keys.c. Names are kept
 * identical to the disassembly except for the one documented rename below.
 *
 * Differences from the sibling iOS image, preserved faithfully:
 *  - NO whitening constant. The iOS image XORed 0x13579BDF into every seed word;
 *    this image seeds the state from the key words directly.
 *  - The checksum acceptance sentinel is 0x00000000 (the iOS image used
 *    0x0000FFFF).
 *  - TWO embedded keys. Key A is the transport/OTA key (seeded directly from the
 *    mask block); Key B is the at-rest device key (seeded by prng_seed_from_key,
 *    which picks either a fixed embedded key or a silicon-derived one). A
 *    boot-time migration re-encrypts Key-A slots to Key-B form.
 *
 * Shared rules across decrypt + checksum (identical to the iOS image):
 *  - The cipher is POSITIONAL: keystream word N is consumed for image word N, so
 *    every generator is advanced for every word even when the word is passed
 *    through unchanged.
 *  - Image words 128..143 (byte offsets 0x200..0x23F, the 64-byte cleartext
 *    header) are passed through VERBATIM so the loader can read magic + length
 *    before it knows the key.
 *  - When the source is XIP flash (top byte 0x14), each <=16-word chunk is
 *    staged into a RAM scratch buffer before processing; a RAM source is used in
 *    place. This is an I/O detail; the result is identical either way.
 *
 * RENAME: the disassembly labels the routine at 0x1400093c as a third
 * "stream_decrypt_skip_header". It is renamed here to
 * stream_reencrypt_keyA_to_keyB because it is not a decryptor: it runs TWO
 * xorshift generators in lockstep (Key A from the mask block, Key B from
 * prng_seed_from_key) and emits out = cipher ^ ksA ^ ksB, converting a Key-A
 * transport image into its Key-B at-rest form. It still preserves the [128..143]
 * header window (the migration must keep the plaintext header), so the original
 * "skip_header" label was accurate as far as it went; "decrypt" hid the dual-key
 * nature and collided with the genuine single-key decryptor below.
 */
#include "bootloader.h"

/* True when an address lies in the memory-mapped SPIFI window (0x14xxxxxx). */
#define IS_XIP(p)   ((((uintptr_t)(p)) & 0xFF000000u) == SPIFI_XIP_BASE)

/* keys.c — the mask/Key-A block and the fixed Key-B block; the build-info block
 * (its first 4 bytes are the marker prng_seed_from_key keys off of). */
extern uint8_t g_key_mask[16];     /* mask block @0x10011EB0: Key A *and* the silicon mask */
extern uint8_t g_keyB[16];         /* fixed Key B @0x10011EC0                              */
extern uint8_t g_build_info[];     /* flash 0x14002958: 4-byte marker + build date/time    */

/* ----------------------------------------------------------------------- */
/* PRNG core                                                               */
/* ----------------------------------------------------------------------- */

/* Textbook 32-bit xorshift128, shift triple (11, 19, 8). State = [x,y,z,w].
 *
 * NOTE: there is no standalone PRNG routine in the image — this step is inlined
 * into every stream routine below. It is written once here, static, purely for
 * readability; the compiler is free to inline it back, reproducing the binary. */
static uint32_t xorshift128_next(uint32_t *state)
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

/* Seed the Key-A state directly from the 16-byte mask block. One-word rotate,
 * NO whitening: state = [k1, k2, k3, k0]. This is the inline seeder used at the
 * Key-A call sites (it has no standalone function in the image). */
static void seed_keyA(uint32_t *state)
{
    const uint32_t *k = (const uint32_t *)g_key_mask;
    state[0] = k[1];   /* x */
    state[1] = k[2];   /* y */
    state[2] = k[3];   /* z */
    state[3] = k[0];   /* w */
}

/* Seed the Key-B state. Reads the 4-byte build marker (big-endian composite);
 * if it exceeds 0x01010000, use the fixed embedded Key B, otherwise derive Key B
 * from silicon = four device-ID dwords at 0x40045000 XORed with the mask block.
 * Same one-word rotate, NO whitening: state = [kB1, kB2, kB3, kB0].
 *
 * In this dump the marker is {01,02,00,00} -> 0x01020000 > 0x01010000, so the
 * FIXED key path is taken. */
uint32_t *prng_seed_from_key(uint32_t *state)
{
    const uint8_t *m = g_build_info;
    uint32_t marker = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16)
                    | ((uint32_t)m[2] << 8)  |  (uint32_t)m[3];
    uint32_t k[4];

    if (marker > BUILD_MARKER_FIXEDKEY) {                    /* fixed Key B */
        const uint32_t *kb = (const uint32_t *)g_keyB;
        k[0] = kb[0]; k[1] = kb[1]; k[2] = kb[2]; k[3] = kb[3];
    } else {                                                 /* silicon-derived Key B */
        const volatile uint32_t *sid  = (const volatile uint32_t *)SILICON_ID_BASE;
        const uint32_t          *mask = (const uint32_t *)g_key_mask;
        k[0] = sid[0] ^ mask[0];
        k[1] = sid[1] ^ mask[1];
        k[2] = sid[2] ^ mask[2];
        k[3] = sid[3] ^ mask[3];
    }
    state[0] = k[1];   /* x */
    state[1] = k[2];   /* y */
    state[2] = k[3];   /* z */
    state[3] = k[0];   /* w */
    return state;
}

/* ----------------------------------------------------------------------- */
/* Checksum (per-key acceptance test)                                      */
/* ----------------------------------------------------------------------- */
/* Decrypt on the fly (header window verbatim) and accumulate two sums: the raw
 * ciphertext sum (vestigial) and the decrypted-word sum. The image is accepted
 * by the caller iff the decrypted sum is exactly 0x00000000.
 *
 * The name says "16" but the accumulators and the acceptance test are full
 * 32-bit. The two variants differ only in key: stream_checksum16 seeds Key A
 * directly from the mask block; stream_checksum16_copy2 seeds Key B via
 * prng_seed_from_key. Both report their two sums through out-params. */
void stream_checksum16(const uint32_t *src, uint32_t byte_len,
                       uint32_t *out_sum_raw, uint32_t *out_sum_dec)
{
    uint32_t state[4];
    uint32_t scratch[16];
    uint32_t total_words, idx;
    uint32_t sum_raw = 0, sum_dec = 0;

    seed_keyA(state);                                   /* Key A */

    if (byte_len == 0) { *out_sum_raw = 0; *out_sum_dec = 0; return; }

    total_words = byte_len >> 2;
    for (idx = 0; idx < total_words; ) {
        uint32_t        chunk = total_words - idx;
        const uint32_t *in;
        if (chunk > 16u) chunk = 16u;
        if (IS_XIP(src)) { memcpy_auto(scratch, src, chunk * 4u); in = scratch; }
        else             { in = src; }
        src += chunk;
        for (uint32_t k = 0; k < chunk; k++, idx++) {
            uint32_t ks    = xorshift128_next(state);
            uint32_t w     = in[k];
            uint32_t plain = (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                             ? w : (w ^ ks);
            sum_raw += w;        /* vestigial raw-ciphertext sum */
            sum_dec += plain;    /* the one that is checked      */
        }
    }
    *out_sum_raw = sum_raw;
    *out_sum_dec = sum_dec;
}
void stream_checksum16_copy2(const uint32_t *src, uint32_t byte_len,
                             uint32_t *out_sum_raw, uint32_t *out_sum_dec)
{
    uint32_t state[4];
    uint32_t scratch[16];
    uint32_t total_words, idx;
    uint32_t sum_raw = 0, sum_dec = 0;

    prng_seed_from_key(state);                          /* Key B */

    if (byte_len == 0) { *out_sum_raw = 0; *out_sum_dec = 0; return; }

    total_words = byte_len >> 2;
    for (idx = 0; idx < total_words; ) {
        uint32_t        chunk = total_words - idx;
        const uint32_t *in;
        if (chunk > 16u) chunk = 16u;
        if (IS_XIP(src)) { memcpy_auto(scratch, src, chunk * 4u); in = scratch; }
        else             { in = src; }
        src += chunk;
        for (uint32_t k = 0; k < chunk; k++, idx++) {
            uint32_t ks    = xorshift128_next(state);
            uint32_t w     = in[k];
            uint32_t plain = (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                             ? w : (w ^ ks);
            sum_raw += w;
            sum_dec += plain;
        }
    }
    *out_sum_raw = sum_raw;
    *out_sum_dec = sum_dec;
}

/* ----------------------------------------------------------------------- */
/* Decrypt (single key, Key B)                                             */
/* ----------------------------------------------------------------------- */
/* Decrypt (or copy) a whole image into dst with a single caller-seeded Key-B
 * keystream. Every ciphertext word is XORed with one keystream word except the
 * 16-word header window [128..143], copied through unchanged. Returns 1 on
 * success, 0 on a bad argument.
 *
 * The disassembly splits this into an entry stub (stream_decrypt_skip_header_
 * entry, 0x140009f8) and the loop body it falls into (stream_decrypt_skip_
 * header_body, 0x140009fe); in C it is one routine. */
int stream_decrypt_skip_header_entry(void *dst, const uint32_t *src,
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
            memcpy_auto(scratch, src, chunk * 4u);
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

/* ----------------------------------------------------------------------- */
/* Re-encrypt (dual key: Key A -> Key B)                                   */
/* ----------------------------------------------------------------------- */
/* The boot-time migration transform (labelled stream_decrypt_skip_header at
 * 0x1400093c in the disassembly; see the rename note at the top). Runs two
 * generators in lockstep and emits out = cipher ^ ksA ^ ksB for every body word,
 * converting a Key-A transport image to its Key-B at-rest form, with the
 * [128..143] header window passed through verbatim. Both generators are seeded
 * internally (ksA from the mask block, ksB via prng_seed_from_key). Returns 1 on
 * success, 0 on a bad argument. */
int stream_reencrypt_keyA_to_keyB(void *dst, const uint32_t *src, uint32_t byte_len)
{
    uint32_t  ksA_state[4];
    uint32_t  ksB_state[4];
    uint32_t  scratch[16];
    uint32_t *out = (uint32_t *)dst;
    uint32_t  total_words, idx;

    if (byte_len == 0) return 1;
    if (src == NULL)   return 0;
    if (dst == NULL)   return 0;
    if (byte_len & 3u) return 0;

    seed_keyA(ksA_state);              /* Key A (transport)  */
    prng_seed_from_key(ksB_state);     /* Key B (at-rest)    */

    total_words = byte_len >> 2;

    for (idx = 0; idx < total_words; ) {
        uint32_t        chunk = total_words - idx;
        const uint32_t *in;

        if (chunk > 16u) chunk = 16u;

        if (IS_XIP(src)) {
            memcpy_auto(scratch, src, chunk * 4u);
            in = scratch;
        } else {
            in = src;
        }
        src += chunk;

        for (uint32_t k = 0; k < chunk; k++, idx++) {
            uint32_t ksA = xorshift128_next(ksA_state);   /* advance BOTH every word */
            uint32_t ksB = xorshift128_next(ksB_state);
            uint32_t w   = in[k];
            if (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                out[idx] = w;                   /* header window: verbatim */
            else
                out[idx] = w ^ ksA ^ ksB;       /* A-form -> B-form        */
        }
    }
    return 1;
}

/* ----------------------------------------------------------------------- */
/* Positional segment decrypt (single key, Key B)                          */
/* ----------------------------------------------------------------------- */
/* Segmented boot path. The PRNG is reseeded by the caller, then fast-forwarded
 * here by image_offset/4 words so the keystream lines up with this segment's
 * absolute position within the overall image. The verbatim window is the 16
 * words [skip_words .. skip_words+15] in absolute image coordinates (skip_words
 * is 0x80 in practice -> the same 0x200..0x23F header window). Returns 1 on
 * success, 0 on a bad argument. Windowing is identical to the iOS image. */
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
            memcpy_auto(scratch, seg_src, chunk * 4u);
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