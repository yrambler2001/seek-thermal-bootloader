/* boot_main.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Flash-resident boot leaves: slot selection, the two single-key acceptance
 * gates, the checksum-acceptance tails, and the word-zeroing helper. The
 * top-level pipeline that drives these lives in startup_lpc43xx.c's
 * Reset_Handler (this image runs the whole boot from flash; there is no
 * separate RAM stage). Names are kept identical to the disassembly except for
 * the documented rename in crypto_stream.c.
 *
 * Two payload differences from the iOS image are visible from here:
 *   - There is NO monolithic path and NO "CODE" footer; the loader always takes
 *     the segmented path (handled in Reset_Handler).
 *   - Validation is split per key. image_try_keys checks Key A only (the
 *     transport/OTA form, used by the boot-time re-encryption migration);
 *     image_try_keys_copy2 checks Key B only (the at-rest device form, used to
 *     choose a bootable slot). Each returns 1 (accept) / 0 (reject) rather than
 *     a key id.
 */

#include "bootloader.h"

/* True when an address lies in the memory-mapped SPIFI window (0x14xxxxxx). */
#define IS_XIP(p)   ((((uintptr_t)(p)) & 0xFF000000u) == SPIFI_XIP_BASE)

/* ----------------------------------------------------------------------- */
void memzero_words(void *dst, uint32_t byte_len)
{
    uint8_t *p = (uint8_t *)dst;
    while (byte_len > (uint32_t)(p - (uint8_t *)dst)) {
        *(uint32_t *)p = 0u;
        p += 4;
    }
}

/* image_checksum_ok / _copy2: the checksum-acceptance tail of the two gates.
 * Given a slot whose cleartext header has already been validated, run the keyed
 * plaintext checksum and accept iff the decrypted-word sum is exactly 0. The
 * key is selected by which checksum routine is called: stream_checksum16 seeds
 * Key A (directly from the mask block); stream_checksum16_copy2 seeds Key B (via
 * prng_seed_from_key). Both routines also return a raw-ciphertext sum through an
 * out-param; it is vestigial here and ignored. */
int image_checksum_ok(const uint32_t *slot_base, uint32_t byte_len)
{
    uint32_t sum_raw, sum_dec;
    stream_checksum16(slot_base, byte_len, &sum_raw, &sum_dec);   /* Key A */
    (void)sum_raw;
    return (sum_dec == ACCEPT_SENTINEL) ? 1 : 0;
}
int image_checksum_ok_copy2(const uint32_t *slot_base, uint32_t byte_len)
{
    uint32_t sum_raw, sum_dec;
    stream_checksum16_copy2(slot_base, byte_len, &sum_raw, &sum_dec); /* Key B */
    (void)sum_raw;
    return (sum_dec == ACCEPT_SENTINEL) ? 1 : 0;
}

/* Acceptance gate, Key A. Reads the cleartext header at slot+0x200, requires
 * magic + length < 0x10000, then runs the Key-A checksum. Returns 1 if the slot
 * is in transport/OTA (Key-A) form, else 0. Used only by the migration sweep. */
int image_try_keys(const uint32_t *slot_base)
{
    uint32_t hdr[16];      /* 64-byte cleartext header window (slot+0x200) */
    if (slot_base == NULL)
        return 0;
    /* slot_base is uint32_t*, so +128 words == +0x200 bytes. */
    memcpy_auto(hdr, slot_base + 128, 0x40u);
    if (hdr[0] != IMG_MAGIC || hdr[1] >= IMG_MAX_LEN)
        return 0;
    return image_checksum_ok(slot_base, hdr[1]);
}

/* Acceptance gate, Key B. Same header check, Key-B checksum. Returns 1 if the
 * slot is in at-rest device (Key-B) form, else 0. This is the gate slot
 * selection uses to decide whether a candidate slot is bootable. */
int image_try_keys_copy2(const uint32_t *slot_base)
{
    uint32_t hdr[16];
    if (slot_base == NULL)
        return 0;
    memcpy_auto(hdr, slot_base + 128, 0x40u);
    if (hdr[0] != IMG_MAGIC || hdr[1] >= IMG_MAX_LEN)
        return 0;
    return image_checksum_ok_copy2(slot_base, hdr[1]);
}

/* Pick a slot from the 16-byte config record at 0x14010000 (its base is also
 * published to the app as g_boot_config_ptr) and return its base address.
 *
 * update_flag==0 is the normal call. update_flag!=0 is the rollback hook driven
 * by the warm-boot mailbox: re-resolve the current slot, and if it is A or B,
 * stamp the flag over that slot's header magic (invalidating it), force the
 * config selector to recovery (2), and return recovery.
 *
 * NOTE the config write differs from the iOS image: that image erased the whole
 * config block and rewrote the 16-byte record; here the selector is updated
 * through the read-modify-write path (flash_program_rmw), which preserves the
 * rest of the block. */
uint32_t select_boot_slot(int update_flag)
{
    uint32_t cfg[4];       /* cfg[0] = selector dword */

    memcpy_auto(cfg, (const void *)BOOT_CONFIG_BASE, 16);

    if (update_flag) {
        uint32_t cur = select_boot_slot(0);
        if (cur == SLOT_A_BASE || cur == SLOT_B_BASE) {
            uint32_t flag = (uint32_t)update_flag;
            uint32_t sel  = 2u;
            /* NOR program only clears bits; this writes the flag over the magic. */
            flash_program((uint8_t *)(cur + IMG_HEADER_OFFSET), &flag, 4u);
            /* selector := recovery, in place (RMW erases + reprograms the block) */
            flash_program_rmw(BOOT_CONFIG_BASE, (const uint8_t *)&sel, 4u);
        }
        return SLOT_RECOVERY_BASE;
    }

    /* selector neither 0 nor 0xFFFFFFFF -> an explicit preference is set */
    if ((uint32_t)(cfg[0] - 1) <= 0xFFFFFFFDu) {
        if (cfg[0] == 1) {                                /* prefer B, then A, then recovery */
            if (!image_try_keys_copy2((const uint32_t *)SLOT_B_BASE)) {
                if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
                    return SLOT_A_BASE;
                return SLOT_RECOVERY_BASE;
            }
        } else {                                          /* prefer recovery, then A, then B */
            if (image_try_keys_copy2((const uint32_t *)SLOT_RECOVERY_BASE))
                return SLOT_RECOVERY_BASE;
            if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
                return SLOT_A_BASE;
        }
    } else {                                              /* blank selector: prefer A, then B, then recovery */
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        if (!image_try_keys_copy2((const uint32_t *)SLOT_B_BASE))
            return SLOT_RECOVERY_BASE;
    }
    return SLOT_B_BASE;
}