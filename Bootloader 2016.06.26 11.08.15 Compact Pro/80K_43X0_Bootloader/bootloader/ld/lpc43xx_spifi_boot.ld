/* CompactPro_43X0_Bootloader/bootloader/ld/lpc43xx_spifi_boot.ld
 * lpc43xx_spifi_boot.ld — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Memory map taken from the dump:
 *   FLASH image  : 0x14000000  external SPI-NOR, memory-mapped (XIP)
 *   App window   : 0x10000000 .. 0x10010000   (64 KB, written at run time)
 *   Boot runtime : 0x10010000 .. 0x10018000   (32 KB: SPIFI driver + data +
 *                                               bss + stack)
 *
 * Distinctive point of this build: the boot LOGIC executes in place from
 * flash (XIP) inside Reset_Handler. The ONLY thing relocated to SRAM is the
 * bespoke SPIFI driver, because driving the controller in command mode drops
 * the flash window out of XIP and the driver cannot fetch itself from it.
 */
ENTRY(Reset_Handler)
MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x14000000, LENGTH = 256K   /* image region used */
    RAM_BOOT (rwx) : ORIGIN = 0x10010000, LENGTH = 32K
    /* 0x10000000..0x10010000 is the 64 KB region the decryptor fills with the
     * application; deliberately NOT an output region here. */
}
__stack_top = 0x10018000;          /* initial MSP (== vector[0]) */
SECTIONS
{
    /* ================= FLASH-resident (executed in place) ================= */
    .vectors 0x14000000 : { KEEP(*(.vectors)) } > FLASH

    /* Scatter-load table walked by Reset_Handler. LPCXpresso "Global Section
     * Table": DATA triplets {LMA,VMA,len} then BSS pairs {addr,len}. In this
     * image the live entries are: copy .data, zero .bss, copy .text_ram. */
    .section_table : ALIGN(4) {
        __data_section_table = .;
            LONG(LOADADDR(.data));     LONG(ADDR(.data));     LONG(SIZEOF(.data));
            LONG(LOADADDR(.text_ram)); LONG(ADDR(.text_ram)); LONG(SIZEOF(.text_ram));
        __data_section_table_end = .;
        __bss_section_table = .;
            LONG(ADDR(.bss));          LONG(SIZEOF(.bss));
        __bss_section_table_end = .;
    } > FLASH

    /* Reset/CRT0 + the entire boot flow + the trap handlers. Stays in flash:
     * it is what relocates the driver below, and it runs from XIP throughout
     * (only stepping out of XIP through the relocated driver). */
    .text_boot : ALIGN(4) {
        *(.text_boot) *(.text_boot.*)
        *(.text .text.*)               /* boot logic + inline literal pools   */
        . = ALIGN(4);
    } > FLASH

    /* Read-only constants that stay in flash: the build-info block (handed to
     * the app as the info pointer) and the device-name strings inside the
     * imported SPIFI driver. */
    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        *(.rodata_flash .rodata_flash.*)
    } > FLASH

    /* ===================== Copied to SRAM (driver only) ==================== */
    /* The bespoke SPIFI driver blob. VMA 0x10010000; LMA 0x14000BE8 in the
     * original (relocation: flash = VMA - 0x10010000 + 0x14000BE8). len 0x1D70. */
    .text_ram : ALIGN(4) {
        __text_ram_start__ = .;
        *(.text_ram .text_ram.*)
        . = ALIGN(4);
        __text_ram_end__ = .;
    } > RAM_BOOT AT> FLASH

    /* Initialised data: the key/mask block, the two embedded keys, the
     * boot-config pointer. VMA 0x10011D70 (== driver VMA + 0x1D70); LMA
     * 0x1400297C in the original. Key A at .data+0x140 (flash 0x14002ABC),
     * Key B at .data+0x150 (flash 0x14002ACC). len 0x160. */
    .data : ALIGN(4) {
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM_BOOT AT> FLASH

    /* The op request struct (0x10011ED0) + driver handle/context (0x10011EE4)
     * + remaining scratch. VMA 0x10011ED0 in the original; len 0x94. */
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