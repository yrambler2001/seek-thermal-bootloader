/* boot_main.c — 80K_4320_Bootloader (reconstructed)
 *
 * Top-level boot stage (executed from SRAM) plus slot selection, the
 * key-agnostic acceptance test, the entry-address sanity check, and the
 * word-zeroing helper. Names are kept identical to the disassembly.
 *
 * Two payload shapes are handled:
 *   - MONOLITHIC: one encrypted image, "CODE" footer, decrypt to 0x10000000,
 *     set MSP/entry from the decrypted vector table, jump.
 *   - SEGMENTED (fallback): decrypt a 0x2C0-byte table, then load 4 segments,
 *     zero 4 BSS regions, load 4 more segments, publish a handoff block, jump.
 */

#include "bootloader.h"
#include "chip.h"      /* LPCOpen: SCB (VTOR), __enable_irq, __WFI */

extern void (* const g_pfnVectors[])(void);   /* bootloader vector table @0x14000000 */

/* SCU pin-config registers for P3_3..P3_8 (SPIFI bus). Base 0x40086000,
 * SFSP3 group at +0x180, one 32-bit reg per pin. */
#define SCU_SFSP3(pin)   (*(volatile uint32_t *)(0x40086000u + 0x180u + ((pin) * 4u)))

/* Launch a decrypted image: load MSP, pass the slot base in R0, branch to the
 * entry. Inline asm so no C stack is touched after MSP is repointed. */
__attribute__((noreturn, used))
static void launch_image(uint32_t sp, uint32_t entry, uint32_t arg)
{
    __asm volatile(
        "msr   msp, %0   \n"
        "mov   r0,  %2   \n"
        "bx    %1        \n"
        : : "r"(sp), "r"(entry), "r"(arg) : "r0", "memory");
    for (;;) {}
}

/* ----------------------------------------------------------------------- */
void memzero_words(void *dst, uint32_t byte_len)
{
    uint8_t *p = (uint8_t *)dst;
    while (byte_len > (uint32_t)(p - (uint8_t *)dst)) {
        *(uint32_t *)p = 0u;
        p += 4;
    }
}

/* Sanity-check a decrypted initial-SP word: it must land in one of the two
 * SRAM windows. Returns 1 (plausible) / 0 (reject). */
int is_allowed_entry_addr(uint32_t addr)
{
    uint32_t base = (addr >> 18) << 18;            /* 256 KB-aligned region base   */
    if (base == 0x10000000u && addr > 0x10018000u)
        return 0;                                   /* low SRAM but past its top    */
    if (base == 0x10080000u)
        return (addr <= 0x1008A000u);               /* second SRAM bank window      */
    return 1;                                        /* any other region: allowed    */
}

/* Acceptance test for one slot. Reads the cleartext header at slot+0x200,
 * requires magic + length<0x10000, then tries Key B/device, then Key A, each
 * via the 16-bit-32-bit checksum followed by an SP plausibility check.
 * Returns 1 (Key B/device), 2 (Key A), 0 (none).
 *
 * NOTE the asymmetry: a PASSING Key-B checksum COMMITS to Key B — if the SP
 * check then fails it returns 0 and does NOT fall through to Key A. */
int image_try_keys(const uint32_t *slot_base)
{
    uint32_t hdr[16];      /* 64-byte cleartext header (slot+0x200) */
    uint32_t state[4];
    uint32_t sp_word[2];

    if (slot_base == NULL)
        return KEYID_NONE;

    /* slot_base is uint32_t*, so +128 words == +0x200 bytes (header window). */
    mem_read_any(hdr, slot_base + 128, 0x40u);
    if (hdr[0] != IMG_MAGIC || hdr[1] >= IMG_MAX_LEN)
        return KEYID_NONE;

    /* Key B / device first. */
    prng_seed_keyB_or_device(state);
    if (stream_checksum16(slot_base, hdr[1], state)) {
        prng_seed_keyB_or_device(state);
        stream_decrypt_skip_header(sp_word, slot_base, 8u, state);
        return is_allowed_entry_addr(sp_word[0]) ? KEYID_B_OR_DEVICE : KEYID_NONE;
    }

    /* Then Key A. */
    prng_seed_keyA(state);
    if (stream_checksum16(slot_base, hdr[1], state)) {
        prng_seed_keyA(state);
        stream_decrypt_skip_header(sp_word, slot_base, 8u, state);
        if (is_allowed_entry_addr(sp_word[0]))
            return KEYID_A;
    }
    return KEYID_NONE;
}

/* Pick a slot from the 16-byte config record at 0x14010000 and return its base.
 * update_flag==0 is the normal call. update_flag!=0 is the rollback hook:
 * it stamps the flag over the live slot's header magic (invalidating it), forces
 * the config selector to 2, rewrites the config block, and returns recovery. */
uint32_t select_boot_slot(int update_flag)
{
    uint32_t cfg[4];       /* cfg[0] = selector dword */

    mem_read(cfg, (const void *)BOOT_CONFIG_BASE, 16);

    if (update_flag) {
        uint32_t cur = select_boot_slot(0);
        if (cur == SLOT_A_BASE || cur == SLOT_B_BASE) {
            /* NOR program only clears bits; this writes flag over the magic. */
            flash_program((uint8_t *)(cur + IMG_HEADER_OFFSET), &update_flag, 4u);
            cfg[0] = 2;
            if (flash_chip_erase((uint8_t *)BOOT_CONFIG_BASE) == FL_OK)
                flash_program((uint8_t *)BOOT_CONFIG_BASE, cfg, 0x10u);
        }
        return SLOT_RECOVERY_BASE;
    }

    /* selector neither 0 nor 0xFFFFFFFF -> an explicit preference is set */
    if ((uint32_t)(cfg[0] - 1) <= 0xFFFFFFFDu) {
        if (cfg[0] == 1) {                               /* prefer B, then A, then recovery */
            if (!image_try_keys((const uint32_t *)SLOT_B_BASE)) {
                if (image_try_keys((const uint32_t *)SLOT_A_BASE))
                    return SLOT_A_BASE;
                return SLOT_RECOVERY_BASE;
            }
        } else {                                          /* prefer recovery, then A, then B */
            if (image_try_keys((const uint32_t *)SLOT_RECOVERY_BASE))
                return SLOT_RECOVERY_BASE;
            if (image_try_keys((const uint32_t *)SLOT_A_BASE))
                return SLOT_A_BASE;
        }
    } else {                                              /* blank selector: prefer A, then B, then recovery */
        if (image_try_keys((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        if (!image_try_keys((const uint32_t *)SLOT_B_BASE))
            return SLOT_RECOVERY_BASE;
    }
    return SLOT_B_BASE;
}

/* ----------------------------------------------------------------------- */
/*
 * Segmented-image table layout (in 32-bit words from the decrypted base).
 * The 0x2C0-byte (176-word) table is decrypted with the header window
 * [128..143] preserved verbatim. Observed word offsets:
 *
 *   [128..143]  cleartext header window; word 132 = app entry pointer
 *   [144..155]  pass-1 load descriptors: 4 x { srcRef, dstVMA, byteLen }
 *   [156..163]  BSS descriptors:         4 x { addr, byteLen }
 *   [164..175]  pass-2 load descriptors: 4 x { srcRef, dstVMA, byteLen }
 *
 * 'srcRef' is a bootloader-relative address; the read offset within the slot is
 * (srcRef - 0x14000000), passed as image_offset to stream_decrypt_segment.
 */
void boot_main(void)
{
    uint32_t state[4];
    uint32_t hdr[16];          /* cleartext header read from slot+0x200 */
    uint32_t segtab[176];      /* decrypted segment table (0x2C0 bytes)  */
    uint32_t slot;
    int      key_id;

    /* Pin-mux P3_3..P3_8 onto the SPIFI bus (boot ROM left a minimal config). */
    SCU_SFSP3(3) = 0xD3u;
    SCU_SFSP3(4) = 0xF3u;
    SCU_SFSP3(5) = 0xD3u;
    SCU_SFSP3(6) = 0xD3u;
    SCU_SFSP3(7) = 0xD3u;
    SCU_SFSP3(8) = 0x13u;

    spifi_init();

    slot = select_boot_slot(0);
    if (slot == 0)
        for (;;) {}                                   /* no bootable slot: park */

    key_id = image_try_keys(slot);                     /* re-resolve the matching key */
    if (key_id == KEYID_B_OR_DEVICE) prng_seed_keyB_or_device(state);
    else                             prng_seed_keyA(state);

    /* Read the cleartext header (this window is stored unencrypted in flash). */
    mem_read(hdr, (const void *)(slot + IMG_HEADER_OFFSET), 64);

    /* ------------------------------- MONOLITHIC ------------------------------- */
    if (hdr[0] == IMG_MAGIC) {
        uint32_t len = hdr[1];
        if (len < IMG_MONOLITHIC_MAX) {
            /* footer at slot + ((len>>14)+1)*0x4000 - 0x40 */
            uint32_t footer_addr = slot + (((len >> 14) + 1u) * 0x4000u) - 0x40u;
            uint32_t footer[16];
            mem_read(footer, (const void *)footer_addr, 64);
            if (footer[0] == IMG_FOOTER_TAG && len == footer[1] &&
                stream_decrypt_skip_header((void *)RAM_APP_LOAD_BASE,
                                           (const uint32_t *)slot, len, state)) {
                SCB->VTOR = RAM_APP_LOAD_BASE;
                launch_image(*(volatile uint32_t *)RAM_APP_LOAD_BASE,        /* MSP   */
                             *(volatile uint32_t *)(RAM_APP_LOAD_BASE + 4),  /* entry */
                             slot);                                          /* R0    */
                /* never returns */
            }
        }
    }

    /* -------------------------------- SEGMENTED ------------------------------- */
    /* State is still the step-above seed (the monolithic path consumes it only
     * on the success path, which jumps away), so the table decrypt uses it. */
    stream_decrypt_skip_header(segtab, (const uint32_t *)slot, SEG_TABLE_BYTES, state);

    /* pass 1: load 4 segments */
    for (int s = 0; s < 4; s++) {
        uint32_t *d   = &segtab[144 + s * 3];
        uint32_t  ref = d[0];
        void     *vma = (void *)d[1];
        uint32_t  len = d[2];
        if (key_id == KEYID_B_OR_DEVICE) prng_seed_keyB_or_device(state);
        else                             prng_seed_keyA(state);
        if (!stream_decrypt_segment(vma, slot, ref - (uint32_t)(uintptr_t)g_pfnVectors,
                                    len, SEG_SKIP_WORDS, state))
            for (;;) {}                                /* decrypt failed: park */
    }

    /* zero 4 BSS regions */
    for (int s = 0; s < 4; s++) {
        uint32_t *d = &segtab[156 + s * 2];
        memzero_words((void *)d[0], d[1]);
    }

    /* pass 2: load 4 more segments */
    for (int s = 0; s < 4; s++) {
        uint32_t *d   = &segtab[164 + s * 3];
        uint32_t  ref = d[0];
        void     *vma = (void *)d[1];
        uint32_t  len = d[2];
        if (key_id == KEYID_B_OR_DEVICE) prng_seed_keyB_or_device(state);
        else                             prng_seed_keyA(state);
        if (!stream_decrypt_segment(vma, slot, ref - (uint32_t)(uintptr_t)g_pfnVectors,
                                    len, SEG_SKIP_WORDS, state))
            for (;;) {}
    }

    /* publish handoff block, hand the bus back, jump the entry */
    mem_read((void *)RAM_APP_LOAD_BASE, segtab, 512);
    *(volatile uint32_t *)(RAM_HANDOFF_INFO + 0x00) = 0x14002F48u;            /* info ptr        */
    *(volatile uint32_t *)(RAM_HANDOFF_INFO + 0x04) = g_boot_config_base;     /* config base     */
    *(volatile uint32_t *)(RAM_HANDOFF_INFO + 0x08) = (slot == SLOT_A_BASE) ? 0u : 2u;
    SCB->VTOR = RAM_APP_LOAD_BASE;
    __enable_irq();
    ((void (*)(int))segtab[132])(0);                   /* entry pointer from table word 132 */
    for (;;) __WFI();                                  /* not reached */
}