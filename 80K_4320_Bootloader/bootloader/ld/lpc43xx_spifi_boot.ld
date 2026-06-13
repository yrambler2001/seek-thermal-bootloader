/* lpc43xx_spifi_boot.ld — 80K_4320_Bootloader (reconstructed)
 *
 * Memory map taken from the dump:
 *   FLASH image  : 0x14000000  external SPI-NOR, memory-mapped (XIP), 64 KB used
 *   App window   : 0x10000000 .. 0x10014000   (80 KB, written at run time)
 *   Boot runtime : 0x10014000 .. 0x10018000   (16 KB: text_ram+data+bss+stack)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x14000000, LENGTH = 64K
    RAM_BOOT (rwx) : ORIGIN = 0x10014000, LENGTH = 16K
    /* 0x10000000..0x10014000 is the 80 KB region the decryptor fills with the
     * application; deliberately NOT an output region here.
     * 0x10014000 == 0x10000000 + 80K  ->  hence the "80K" in the build name. */
}

__stack_top = 0x10018000;          /* initial MSP (== vector[0]) */

SECTIONS
{
    /* ================= FLASH-resident (executed in place) ================= */

    .vectors 0x14000000 : { KEEP(*(.vectors)) } > FLASH

    /* Reserved per-unit key slot — pinned at 0x14000218. */
    .dev_key 0x14000218 : { KEEP(*(.dev_key)) } > FLASH

    /* Reset/CRT0 + trap handlers + jump_to_ram_stage. Stays in flash because
     * it is what relocates everything below. */
    .text_boot : ALIGN(4) {
        *(.text_boot) *(.text_boot.*)
    } > FLASH

    /* Scatter-load table walked by Reset_Handler (LPCXpresso "Global Section
     * Table": DATA triplets {LMA,VMA,len}, then BSS pairs {addr,len}). */
    .section_table : ALIGN(4) {
        __data_section_table = .;
            LONG(LOADADDR(.text_ram)); LONG(ADDR(.text_ram)); LONG(SIZEOF(.text_ram));
            LONG(LOADADDR(.data));     LONG(ADDR(.data));     LONG(SIZEOF(.data));
        __data_section_table_end = .;
        __bss_section_table = .;
            LONG(ADDR(.bss));          LONG(SIZEOF(.bss));
        __bss_section_table_end = .;
    } > FLASH

    /* Read-only constants stay in flash (e.g. the lpcspifilib device-name
     * strings; the descriptor STRUCTS that get list-linked at run time are
     * writable and therefore land in .data below). */
    .rodata : ALIGN(4) { *(.rodata .rodata.*) } > FLASH

    /* Build-identity strings / footer markers. */
    .rodata_flash : ALIGN(4) { *(.rodata_flash .rodata_flash.*) } > FLASH

    /* ================= Copied to SRAM, executed from SRAM ================= */

    /* boot_main + crypto + flash layer + lpcspifilib + util — everything
     * EXCEPT .text_boot. VMA starts at 0x10014000; LMA trails the section
     * table in flash (0x140003B8 in the original). */
    .text_ram : ALIGN(4) {
        __text_ram_start__ = .;
        *(.text_ram .text_ram.*)
        *(.text .text.*)               /* code + inline literal pools         */
        . = ALIGN(4);
        __text_ram_end__ = .;
    } > RAM_BOOT AT> FLASH

    /* Initialised data: g_boot_config_base, g_keyA, g_keyB, plus the
     * lpcspifilib device database (its nodes are linked into a list at run
     * time, so they must be writable). VMA == 0x10016B90 in the original. */
    .data : ALIGN(4) {
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM_BOOT AT> FLASH

    /* lpcspifilib scratch + device handle + init/lock refcounts + BSS.
     * VMA == 0x10016FFC in the original (the table the BSS pass zeroes). */
    .bss (NOLOAD) : ALIGN(4) {
        __bss_start__ = .;
        *(.spifi_bss .spifi_bss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM_BOOT

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}