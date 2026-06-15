/* startup_lpc43xx.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Reset/CRT0 + vector table for the Seek Thermal Compact Pro (Android) LPC43xx
 * boot image. The image embeds no internal name string; it is stamped only with
 * a 4-byte build marker {01,02,00,00} and the build date/time "Jun 26 2016"
 * "11:08:15" (at flash 0x14002958). The workspace/folder name is therefore
 * descriptive, anchored to the product code (UQ-AAA) and that build date.
 *
 * FLASH-resident bootstrap. Unlike the sibling iOS image — which relocated ALL
 * of its code into SRAM and ran a separate boot_main stage there — this build
 * executes its ENTIRE boot pipeline in place (XIP) from 0x14000000, inside
 * Reset_Handler. The ONLY thing relocated into SRAM is the bespoke SPIFI driver
 * blob (flash 0x14000BE8 -> RAM 0x10010000, 0x1D70 bytes); see spifi_glue.c.
 * When the boot pipeline needs command-mode flash access (program/erase, which
 * drops XIP) it calls flash-resident thunks that branch into that RAM-resident
 * driver. Because the boot logic itself never leaves flash, every leaf routine
 * Reset_Handler calls is also flash-resident.
 *
 * The vector table is the stock LPCOpen shape: the core-fault and system slots
 * each point at their own tiny spin handler, and every one of the ~53 external
 * peripheral IRQ slots points at the single shared trap IRQ52_Handler (the
 * linker/disassembler names the shared default-handler body after one of its
 * slots). NOTE the difference from the iOS image: that image installed three
 * live RAM-resident handlers on IRQ12/13/14; this image installs NONE — every
 * peripheral vector is the trap. The reserved 0 entries are NXP's reserved
 * vector positions. We keep the disassembly's IRQ52 name rather than reintroduce
 * the weak *_IRQHandler aliases, so source and listing line up exactly.
 *
 * The core faults spin with a plain "B ." (an empty for(;;)), NOT "WFI; B ." —
 * another divergence from the iOS image, whose fault handlers executed WFI.
 */
#include "bootloader.h"
#include "chip.h"      /* LPCOpen: LPC_CREG (M4MEMMAP), LPC_RGU, SCB (VTOR),
                          NVIC (ICPR), __set_MSP, __disable_irq, __enable_irq   */

#define BOOT_TEXT __attribute__((section(".text_boot")))   /* stays in flash (XIP) */

/* ------------------------------- handlers ------------------------------- */
/* Core faults: plain spin ("B ."), no WFI. Each is its own function so the
 * vector table can point at it individually (it does — see g_pfnVectors[]).   */
BOOT_TEXT __attribute__((noreturn)) void NMI_Handler(void)        { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void HardFault_Handler(void)  { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void MemManage_Handler(void)  { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void BusFault_Handler(void)   { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void UsageFault_Handler(void) { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void SVC_Handler(void)        { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void DebugMon_Handler(void)   { for (;;) {} }
BOOT_TEXT __attribute__((noreturn)) void PendSV_Handler(void)     { for (;;) {} }

BOOT_TEXT __attribute__((noreturn)) void SysTick_Handler(void)    { for (;;) {} }

/* Shared trap for every unused peripheral IRQ: plain spin. */
BOOT_TEXT __attribute__((noreturn)) void IRQ52_Handler(void)      { for (;;) {} }

/* --------------------------- vector table ------------------------------ */
extern void Reset_Handler(void);

void (* const g_pfnVectors[])(void) __attribute__((section(".vectors"), used)) =
{
    (void (*)(void))MSP_TOP,   /*  0  initial SP  = 0x10018000           */
    Reset_Handler,             /*  1  reset       (0x14000264, Thumb)    */
    NMI_Handler,               /*  2                                      */
    HardFault_Handler,         /*  3                                      */
    MemManage_Handler,         /*  4                                      */
    BusFault_Handler,          /*  5                                      */
    UsageFault_Handler,        /*  6                                      */
    0, 0, 0, 0,                /*  7..10  reserved                        */
    SVC_Handler,               /* 11                                      */
    DebugMon_Handler,          /* 12                                      */
    0,                         /* 13  reserved                            */
    PendSV_Handler,            /* 14                                      */
    SysTick_Handler,           /* 15                                      */
    /* -------- external interrupts IRQ0..IRQ52 (LPC43xx M4) --------      */
    /* No live peripheral handlers in this image: every slot is the trap.  */
    IRQ52_Handler, IRQ52_Handler,                               /* 0,1    */
    0, 0,                                                       /* 2,3  r */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 4..7   */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 8..11  */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 12..15 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 16..19 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 20..23 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 24..27 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 28..31 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 32..35 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 36..39 */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 40..43 */
    0,                                                          /* 44   r */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler,                /* 45..47 */
    0,                                                          /* 48   r */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 49..52 */
};

/* ----------------- scatter-load tables (from the .ld) ------------------ */
/* LPCXpresso "Global Section Table" layout: {LMA,VMA,len} copy triplets,
 * then {addr,len} zero pairs. On this image the triplet list is, in order:
 *   1) .data            : flash 0x1400297C -> 0x10011D70, len 0x160 (keys/cfg)
 *   2) .text_ram driver : flash 0x14000BE8 -> 0x10010000, len 0x1D70 (the blob)
 * and the zero list is:
 *   1) .bss             : 0x10011ED0, len 0x94 (request struct + driver ctx)   */
extern uint32_t __data_section_table[];      /* { LMA, VMA, len } triplets */
extern uint32_t __data_section_table_end[];
extern uint32_t __bss_section_table[];        /* { addr, len } pairs        */
extern uint32_t __bss_section_table_end[];

/* Word-copy CRT0 helper (flash-resident). The sibling iOS image inlined its
 * scatter-load loops and had to fence them with no-tree-loop-distribute-patterns
 * so GCC would not rewrite them into memcpy/memset calls that resolve into
 * not-yet-relocated SRAM. This image instead factors the copy into an explicit
 * flash-resident routine, so there is no inline idiom for GCC to recognise and
 * no barrier is needed: scatterload_copy_words and memzero_words both live in
 * flash and are safe to call at any point during CRT0.                          */
BOOT_TEXT void scatterload_copy_words(uint32_t *dst, const uint32_t *src, uint32_t byte_len)
{
    for (uint32_t off = 0; off < byte_len; off += 4)
        dst[off >> 2] = src[off >> 2];
}

/* RAM warm-boot update mailbox (written by a previously running application
 * before it triggered a reset; lives in the application SRAM region, which the
 * scatter-load does not touch, so it survives across CRT0). */
#define MAILBOX_FLAG   (*(volatile uint32_t *)RAM_UPDATE_FLAG)    /* 0x1000020C */
#define MAILBOX_MAGIC  (*(volatile uint64_t *)RAM_UPDATE_MAGIC)   /* 0x10000210 */

/* keys.c — placed at fixed addresses by the linker (see ld script). */
extern uint8_t  g_build_info[];        /* flash 0x14002958: marker + build date */
extern uint32_t g_boot_config_ptr;     /* boot-config base value (0x14010000)   */

/*
 * Segmented-image table layout (32-bit word offsets from the decrypted base),
 * identical to the iOS image. The 0x2C0-byte (176-word) table is decrypted with
 * the header window [128..143] preserved verbatim. Observed word offsets:
 *
 *   [128..143]  cleartext header window; word 132 = app entry pointer
 *   [144..155]  pass-1 load descriptors: 4 x { srcRef, dstVMA, byteLen }
 *   [156..163]  BSS descriptors:         4 x { addr, byteLen }
 *   [164..175]  pass-2 load descriptors: 4 x { srcRef, dstVMA, byteLen }
 *
 * 'srcRef' is an image-base-relative address; the read offset within the slot
 * is (srcRef - 0x14000000), passed as image_offset to stream_decrypt_segment.
 *
 * NOTE: there is NO monolithic path and NO "CODE" footer here (both present in
 * the iOS image) — this image goes straight to the segmented loader.
 */

/* Reset_Handler is the whole show: CRT0 + warm-boot mailbox + SPIFI bring-up +
 * the Key-A -> Key-B re-encryption migration + slot selection + the segmented
 * decrypt/load + handoff. It runs entirely from flash and never returns.        */
BOOT_TEXT __attribute__((noreturn))
void Reset_Handler(void)
{
    uint32_t *t;

    /* ---------------------------- CRT0 prologue --------------------------- */
    __disable_irq();                                /* CPSID i               */

    /* Alias the 0x10000000 SRAM bank to address 0 (so the app we will load at
     * 0x10000000 is also reachable via the legacy 0 view); keep the live vector
     * table in flash for now. VTOR is flipped to 0x10000000 just before launch. */
    LPC_CREG->M4MEMMAP = RAM_APP_LOAD_BASE;         /* 0x10000000            */
    SCB->VTOR          = (uint32_t)g_pfnVectors;    /* 0x14000000            */

    /* Stack guard canary at MSP-0x800 (== 0x10017800), then (re)arm MSP. The
     * hardware already loaded MSP from vector[0] at reset; the image re-sets it
     * explicitly to the same value after planting the canary. Same 0xCDCDCDCD
     * sentinel as the sibling image (which placed it at 0x1001F800).            */
    *(volatile uint32_t *)(MSP_TOP - 0x800u) = 0xCDCDCDCDu;   /* 0x10017800   */
    __set_MSP(MSP_TOP);                                       /* 0x10018000   */

    /* Pulse-reset peripherals left configured by the boot ROM, then clear every
     * pending interrupt. (The RESET_CTRL masks match the sibling image's
     * sequence; if this revision differs only the two literals change.)         */
    LPC_RGU->RESET_CTRL[0] = 0x10DF1000u;
    LPC_RGU->RESET_CTRL[1] = 0x01DFF7FFu;
    for (int i = 0; i < 8; i++)
        NVIC->ICPR[i] = 0xFFFFFFFFu;                /* ICPR0..ICPR7          */

    /* ---------------------------- scatter-load ---------------------------- */
    /* Copy the {LMA,VMA,len} triplets: .data (keys/config) then the SPIFI
     * driver blob into SRAM. Self-copies (src==dst) are skipped, as in the
     * original. Both this loop's copy helper and the BSS-zero helper are
     * flash-resident, so they are callable here even though the SRAM copy of
     * the driver does not exist yet.                                            */
    for (t = __data_section_table; t < __data_section_table_end; ) {
        const uint32_t *src = (const uint32_t *)*t++;
        uint32_t       *dst = (uint32_t *)*t++;
        uint32_t        len = *t++;
        if (src != dst)
            scatterload_copy_words(dst, src, len);
    }
    /* Zero the {addr,len} BSS pairs (the SPIFI request struct + driver ctx). */
    for (t = __bss_section_table; t < __bss_section_table_end; ) {
        uint32_t *dst = (uint32_t *)*t++;
        uint32_t  len = *t++;
        memzero_words(dst, len);                    /* flash-resident leaf   */
    }

    /* ----------------------- warm-boot update mailbox --------------------- */
    /* Honoured only if the flag is one of the two update magics
     * (0xAA55FF01 / 0xAA55FF02 -> (flag + 0x55AA00FF) <= 1) AND the companion
     * 64-bit value is <= 0x752F. When valid, derive a non-zero update request
     * for select_boot_slot (the rollback hook), then clear the mailbox so the
     * request fires exactly once. The exact flag->request mapping is inferred:
     * 0xAA55FF01 -> 1, 0xAA55FF02 -> 2.                                         */
    int update_flag = 0;
    {
        uint32_t flag  = MAILBOX_FLAG;
        uint64_t gate  = MAILBOX_MAGIC;
        if ((flag + 0x55AA00FFu) <= 1u && gate <= (uint64_t)UPDATE_MAGIC_GATE) {
            update_flag = (int)(flag - UPDATE_FLAG_A + 1u);   /* 0xAA55FF01->1 */
            MAILBOX_FLAG  = 0u;
            MAILBOX_MAGIC = 0u;
        }
    }

    /* ----------------------------- SPIFI bring-up ------------------------- */
    /* Pin-mux + fill the request struct + bring the driver up into XIP. */
    spifi_init();

    /* ----------- Key-A -> Key-B re-encryption migration (every boot) ------ */
    /* Walk all three slots. A slot still in transport/OTA form validates under
     * Key A (image_try_keys); convert it in place to the device-storage Key-B
     * form and write it back, so subsequent boots see a Key-B slot and load it
     * normally. Key A is the transport key; Key B is the at-rest device key.
     *
     * stream_reencrypt_keyA_to_keyB runs two xorshift generators in lockstep —
     * ksA seeded from the mask block (Key A) and ksB seeded from Key B — and
     * emits out = cipher ^ ksA ^ ksB for every body word, while passing the
     * [128..143] header window through verbatim (the migration MUST keep the
     * plaintext header). See crypto_stream.c.                                   */
    {
        static const uint32_t slot_bases[3] =
            { SLOT_A_BASE, SLOT_B_BASE, SLOT_RECOVERY_BASE };
        for (int s = 0; s < 3; s++) {
            uint32_t slot = slot_bases[s];
            if (image_try_keys((const uint32_t *)slot)) {   /* Key-A: transport? */
                uint32_t hdr[16];
                uint32_t len;
                memcpy_auto(hdr, (const void *)(slot + IMG_HEADER_OFFSET), 0x40u);
                if (hdr[0] != IMG_MAGIC || hdr[1] >= IMG_MAX_LEN)
                    continue;
                len = hdr[1];
                /* stage the ciphertext, re-key it in place to the Key-B form */
                memcpy_auto((void *)STAGING_BUF_BASE, (const void *)slot, len);
                stream_reencrypt_keyA_to_keyB((void *)STAGING_BUF_BASE,
                                              (const uint32_t *)STAGING_BUF_BASE, len);
                /* erase the slot's 64K block, then write the Key-B image back */
                if (flash_chip_erase((uint8_t *)slot) == FL_OK)
                    flash_program((uint8_t *)slot, (const void *)STAGING_BUF_BASE, len);
            }
        }
    }

    /* ----------------------------- pick a slot ---------------------------- */
    uint32_t slot = select_boot_slot(update_flag);
    if (slot == 0)
        for (;;) {}                                 /* no bootable slot: park */

    /* --------------------------- segmented load --------------------------- */
    /* Decrypt the 0x2C0-byte segment table under Key B (header window kept
     * verbatim), then load two passes of four segments with a BSS-zero pass
     * between them. The PRNG is reseeded per segment and fast-forwarded inside
     * stream_decrypt_segment to align the keystream to each segment's absolute
     * position within the image.                                               */
    uint32_t segtab[176];
    uint32_t state[4];

    prng_seed_from_key(state);                      /* Key B                 */
    stream_decrypt_skip_header_entry(segtab, (const uint32_t *)slot,
                                     SEG_TABLE_BYTES, state);

    /* pass 1: load descriptors [144..155] */
    for (int i = 0; i < 4; i++) {
        uint32_t *d = &segtab[144 + i * 3];         /* { srcRef, dstVMA, len } */
        prng_seed_from_key(state);
        if (!stream_decrypt_segment((void *)d[1], slot, d[0] - SPIFI_XIP_BASE,
                                    d[2], SEG_SKIP_WORDS, state))
            for (;;) {}                             /* decrypt failed: park  */
    }
    /* zero 4 BSS regions [156..163] */
    for (int i = 0; i < 4; i++) {
        uint32_t *d = &segtab[156 + i * 2];         /* { addr, len }         */
        memzero_words((void *)d[0], d[1]);
    }
    /* pass 2: load descriptors [164..175] */
    for (int i = 0; i < 4; i++) {
        uint32_t *d = &segtab[164 + i * 3];         /* { srcRef, dstVMA, len } */
        prng_seed_from_key(state);
        if (!stream_decrypt_segment((void *)d[1], slot, d[0] - SPIFI_XIP_BASE,
                                    d[2], SEG_SKIP_WORDS, state))
            for (;;) {}
    }

    /* ------------------------------- handoff ------------------------------ */
    /* Install the app's vector table (first 0x200 bytes of the decrypted table)
     * at 0x10000000, publish the handoff block, point VTOR at the app, enable
     * interrupts, and jump the entry. The slot indicator distinguishes all
     * three slots (0=A, 1=B, 2=recovery) — the iOS image only reported 0 vs 2.  */
    memcpy_auto((void *)RAM_APP_LOAD_BASE, segtab, 0x200u);

    *(volatile uint32_t *)(RAM_HANDOFF_BLOCK + 0x00) =
        (uint32_t)(uintptr_t)g_build_info;          /* info ptr  (0x14002958)  */
    *(volatile uint32_t *)(RAM_HANDOFF_BLOCK + 0x04) =
        g_boot_config_ptr;                          /* config base (0x14010000)*/
    *(volatile uint32_t *)(RAM_HANDOFF_BLOCK + 0x08) =
        (slot == SLOT_A_BASE) ? (uint32_t)SLOTID_A
      : (slot == SLOT_B_BASE) ? (uint32_t)SLOTID_B
      :                         (uint32_t)SLOTID_RECOVERY;

    SCB->VTOR = RAM_APP_LOAD_BASE;                  /* 0x10000000            */
    __enable_irq();                                 /* CPSIE i               */

    ((void (*)(void))segtab[132])();                /* entry from table word 132 */

    for (;;) {}                                     /* not reached           */
}