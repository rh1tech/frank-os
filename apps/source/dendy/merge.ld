/* Linker script to merge per-function C++ sections into standard sections.
 * Used with: arm-none-eabi-ld -r --force-group-allocation -T merge.ld */
SECTIONS
{
    . = 0;
    .text : { *(.text .text.* .time_critical.* .gnu.linkonce.t.*) }
    . = 0;
    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
    . = 0;
    .data : { *(.data .data.* .gnu.linkonce.d.*) }
    . = 0;
    .bss : { *(.bss .bss.* .gnu.linkonce.b.* COMMON) }
}
