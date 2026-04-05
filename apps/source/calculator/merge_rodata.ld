/* Merge all .rodata subsections into a single .rodata section.
 * GCC puts string literals in .rodata.str1.4 which is a separate
 * section when using -r (relocatable).  Some ELF loaders may not
 * handle the separate section correctly. */
SECTIONS {
    .rodata : {
        *(.rodata .rodata.*)
    }
}
