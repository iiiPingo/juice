// Tests for the widget themes (theme.cpp). Build and run: tests\run.cmd (exit code 0 = passed).
#include "../src/common.h"
#include <cstdio>

static int fails = 0;
static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int wmain() {
    Check(ParseTheme(L"dark") == ThemeMode::Dark, "dark");
    Check(ParseTheme(L"oled") == ThemeMode::Oled, "oled");
    Check(ParseTheme(L"light") == ThemeMode::Light, "light");
    Check(ParseTheme(L"  OLED ") == ThemeMode::Oled, "case and spaces ignored");
    Check(ParseTheme(L"") == ThemeMode::Dark, "empty = dark");
    Check(ParseTheme(L"purple") == ThemeMode::Dark, "junk = dark");
    Check(ParseTheme(nullptr) == ThemeMode::Dark, "null = dark");
    for (ThemeMode m : { ThemeMode::Dark, ThemeMode::Oled, ThemeMode::Light })
        Check(ParseTheme(ThemeName(m)) == m && PaletteFor(m).mode == m, "name round trip");

    const Palette& d = PaletteFor(ThemeMode::Dark);
    Check(d.bg == RGB(0x20, 0x20, 0x20) && d.field == RGB(0x2D, 0x2D, 0x2D) && d.track == RGB(0x3A, 0x3A, 0x3A) &&
          d.fill == RGB(0xD8, 0xD8, 0xD8) && d.glyph == RGB(0xF2, 0xF2, 0xF2) && d.border == CLR_INVALID && !d.light,
          "dark is the original look");
    const Palette& o = PaletteFor(ThemeMode::Oled);
    Check(o.bg == RGB(0, 0, 0) && o.border == RGB(0x1F, 0x1F, 0x24) && !o.light, "oled: pure black, #1F1F24 outline");
    const Palette& l = PaletteFor(ThemeMode::Light);
    Check(l.bg == RGB(0xF3, 0xF3, 0xF3) && l.text == RGB(0x1B, 0x1B, 0x1B) && l.border == RGB(0xE0, 0xE0, 0xE0) && l.light,
          "light: Windows 11 light, dark text");
    // every theme: text, glyphs and bars stand out from the background
    auto lum = [](COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; };
    for (ThemeMode m : { ThemeMode::Dark, ThemeMode::Oled, ThemeMode::Light }) {
        const Palette& p = PaletteFor(m);
        Check(abs(lum(p.text) - lum(p.bg)) > 150 && abs(lum(p.pct) - lum(p.bg)) > 150 && abs(lum(p.glyph) - lum(p.bg)) > 150, "text contrast");
        Check(abs(lum(p.fill) - lum(p.track)) > 60 && lum(p.track) != lum(p.bg), "bar visible on its track");
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
