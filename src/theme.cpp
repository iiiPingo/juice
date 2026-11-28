// Widget themes: Dark (default, the original look), OLED (pure black) and Light (Windows 11 light).
// Same three looks as Minibar's Theme picker. Saved as Theme=dark|oled|light in settings.ini.
#include "common.h"

ThemeMode ParseTheme(const wchar_t* s) {
    if (!s) return ThemeMode::Dark;
    std::wstring v(s);
    while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    size_t start = v.find_first_not_of(L" \t");
    v = start == std::wstring::npos ? L"" : v.substr(start);
    for (auto& c : v) c = (wchar_t)towlower(c);
    if (v == L"oled") return ThemeMode::Oled;
    if (v == L"light") return ThemeMode::Light;
    return ThemeMode::Dark;   // empty, unknown or hand-edited junk: the default
}

const wchar_t* ThemeName(ThemeMode m) {
    return m == ThemeMode::Oled ? L"oled" : m == ThemeMode::Light ? L"light" : L"dark";
}

const Palette& PaletteFor(ThemeMode m) {
    // Dark: the colours Juice always drew with, value for value.
    static const Palette dark = {
        ThemeMode::Dark, false,
        RGB(0x20, 0x20, 0x20), RGB(0x2D, 0x2D, 0x2D), CLR_INVALID,   // bg, field, border (system)
        RGB(0xF2, 0xF2, 0xF2), RGB(0xA0, 0xA0, 0xA0),                 // text, sub
        RGB(0xF2, 0xF2, 0xF2), RGB(0x70, 0x70, 0x70),                 // glyph, glyph asleep
        RGB(0xFF, 0xFF, 0xFF),                                        // percent
        RGB(0xF0, 0xF0, 0xF0), RGB(0x90, 0x90, 0x90),                 // name, name asleep
        RGB(0x3A, 0x3A, 0x3A), RGB(0xD8, 0xD8, 0xD8), RGB(0x6A, 0x6A, 0x6A),   // track, fill, fill asleep
        RGB(0xC8, 0xC8, 0xC8), RGB(0x70, 0x70, 0x70),                 // earbud parts, asleep
        RGB(0x8C, 0x8C, 0x8C),                                        // moon
    };
    // OLED (Discord Onyx style, as in Minibar): pure black, near-black field, #1F1F24 outline.
    static const Palette oled = {
        ThemeMode::Oled, false,
        RGB(0x00, 0x00, 0x00), RGB(0x13, 0x13, 0x16), RGB(0x1F, 0x1F, 0x24),
        RGB(0xF2, 0xF2, 0xF2), RGB(0xA0, 0xA0, 0xA0),
        RGB(0xF2, 0xF2, 0xF2), RGB(0x60, 0x60, 0x60),
        RGB(0xFF, 0xFF, 0xFF),
        RGB(0xF0, 0xF0, 0xF0), RGB(0x88, 0x88, 0x88),
        RGB(0x2A, 0x2A, 0x30), RGB(0xD8, 0xD8, 0xD8), RGB(0x55, 0x55, 0x5C),
        RGB(0xC8, 0xC8, 0xC8), RGB(0x60, 0x60, 0x60),
        RGB(0x80, 0x80, 0x80),
    };
    // Light (Windows 11 Fluent, as in Minibar): #F3F3F3 canvas, white field, #E0E0E0 outline, #1B1B1B text.
    static const Palette light = {
        ThemeMode::Light, true,
        RGB(0xF3, 0xF3, 0xF3), RGB(0xFF, 0xFF, 0xFF), RGB(0xE0, 0xE0, 0xE0),
        RGB(0x1B, 0x1B, 0x1B), RGB(0x5C, 0x5C, 0x5C),
        RGB(0x1B, 0x1B, 0x1B), RGB(0xA0, 0xA0, 0xA0),
        RGB(0x1B, 0x1B, 0x1B),
        RGB(0x1B, 0x1B, 0x1B), RGB(0x8A, 0x8A, 0x8A),
        RGB(0xDC, 0xDC, 0xDC), RGB(0x5C, 0x5C, 0x5C), RGB(0xB8, 0xB8, 0xB8),
        RGB(0x5C, 0x5C, 0x5C), RGB(0xA0, 0xA0, 0xA0),
        RGB(0x8A, 0x8A, 0x8A),
    };
    return m == ThemeMode::Oled ? oled : m == ThemeMode::Light ? light : dark;
}
