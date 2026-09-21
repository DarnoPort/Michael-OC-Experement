#ifndef MICHAEL_OS_TERMINAL_FONT_H
#define MICHAEL_OS_TERMINAL_FONT_H

struct terminal_font_glyph {
    unsigned char code;
    unsigned char bitmap[16];
};

extern const struct terminal_font_glyph terminal_cyrillic_glyphs[];
extern const unsigned int terminal_cyrillic_glyph_count;

#endif
