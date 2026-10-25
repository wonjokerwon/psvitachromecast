/* vita-elf-create appends the module import/export tables (a few KB) right
 * after the text segment. The default linker script starts the data segment
 * at the next 64 KiB boundary, so when the code happens to end just below
 * one there is no room and the build fails with "segment 1 overlaps".
 * Leaving a fixed gap after the text segment makes the layout always fit. */
SECTIONS
{
  . = . + 0x4000;
}
INSERT AFTER .eh_frame;
