ENTRY(user_program_start)

PHDRS
{
    user_text PT_LOAD FLAGS(5);
}

SECTIONS
{
    . = 0x80000000;

    .text : ALIGN(0x1000)
    {
        *(.text)
    } :user_text

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame)
    }
}
