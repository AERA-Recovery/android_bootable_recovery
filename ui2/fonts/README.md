# AERA Latin font fallback

These generated LVGL fonts add `U+00A0-U+017F` and `U+20AC` to the
built-in Montserrat sizes used by UI2. ASCII and LVGL symbols remain in
LVGL's built-in fonts; `aera_latin_*.c` is used only as a glyph fallback.

The source font is LVGL's `scripts/built_in_font/Montserrat-Medium.ttf`.
Regenerate each size with `lv_font_conv` using 4 bpp, no compression,
no prefilter, fast kerning, and this range:

```text
0xA0-0x17F,0x20AC
```
