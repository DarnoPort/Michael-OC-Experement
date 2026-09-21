ENTRY(user_program_start)

PHDRS
{
    user_text PT_LOAD FLAGS(5); /* Read + Execute */
    user_data PT_LOAD FLAGS(6); /* Read + Write */
}

SECTIONS
{
    . = 0x80000000;

    /*
     * Keep executable code on its own page so the loader can make
     * it non-writable without also making .bss writable.
     */
    .text : ALIGN(0x1000)
    {
        *(.text)
    } :user_text

    . = ALIGN(0x1000);

    /*
     * Writable process data. The current demo programs use .bss for
     * mutable buffers, so it must live in a writable PT_LOAD segment.
     */
    .data :
    {
        *(.data)
    } :user_data

    .bss :
    {
        *(.bss)
        *(COMMON)
    } :user_data

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
