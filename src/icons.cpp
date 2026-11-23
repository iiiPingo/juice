// Tray icons: a device symbol (AirPods, headphones, headset, mouse, controller) inside a ring
// that fills with the battery level. Shapes are drawn with GDI+ into separate coverage masks
// (body, holes) and composited by hand, so cut-outs stay anti-aliased and transparent.
#include "common.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

using namespace Gdiplus;

namespace {

ULONG_PTR g_gdipToken = 0;

// A coverage mask (0..255 per pixel) drawn by GDI+.
struct Mask {
    int w, h;
    std::vector<BYTE> a;
    Mask(int w_, int h_) : w(w_), h(h_), a((size_t)w_ * h_, 0) {}
};

// Unit-square coordinates -> pixels inside `box`.
struct Box {
    REAL x, y, s;
    REAL X(REAL u) const { return x + u * s; }
    REAL Y(REAL v) const { return y + v * s; }
    REAL L(REAL d) const { return d * s; }
};

void AddRoundRect(GraphicsPath& p, const Box& b, REAL x0, REAL y0, REAL x1, REAL y1, REAL r) {
    REAL X0 = b.X(x0), Y0 = b.Y(y0), X1 = b.X(x1), Y1 = b.Y(y1);
    REAL d = std::min(b.L(r) * 2, std::min(X1 - X0, Y1 - Y0));
    p.StartFigure();
    if (d <= 0.5f) {
        p.AddRectangle(RectF(X0, Y0, X1 - X0, Y1 - Y0));
        return;
    }
    p.AddArc(X0, Y0, d, d, 180, 90);
    p.AddArc(X1 - d, Y0, d, d, 270, 90);
    p.AddArc(X1 - d, Y1 - d, d, d, 0, 90);
    p.AddArc(X0, Y1 - d, d, d, 90, 90);
    p.CloseFigure();
}

void AddEllipse(GraphicsPath& p, const Box& b, REAL cx, REAL cy, REAL rx, REAL ry) {
    p.StartFigure();
    p.AddEllipse(b.X(cx - rx), b.Y(cy - ry), b.L(rx * 2), b.L(ry * 2));
}

// Renders body and holes for one device symbol into the masks. Small sizes (tray) keep only the
// cut-outs that stay readable; the widget's larger symbols get the full detail.
void DrawGlyph(Kind kind, const Box& b, Graphics& body, Graphics& holes) {
    SolidBrush white(Color(255, 255, 255, 255));
    GraphicsPath p(FillModeWinding), h(FillModeWinding);
    const bool detail = b.s >= 18;
    switch (kind) {
        case Kind::Mouse:
            AddRoundRect(p, b, 0.21f, 0.03f, 0.79f, 0.97f, 0.29f);
            AddRoundRect(h, b, 0.455f, 0.15f, 0.545f, 0.38f, 0.045f);  // scroll wheel
            if (detail) {
                AddRoundRect(h, b, 0.485f, 0.00f, 0.515f, 0.15f, 0.0f);   // button split
                AddRoundRect(h, b, 0.21f, 0.475f, 0.79f, 0.505f, 0.0f);   // palm line
            }
            break;
        case Kind::Keyboard: {
            AddRoundRect(p, b, 0.02f, 0.21f, 0.98f, 0.79f, 0.11f);
            int cols = detail ? 7 : 5;
            REAL left = 0.10f, span = 0.80f, gap = detail ? 0.028f : 0.04f;
            REAL w = (span - gap * (cols - 1)) / cols;
            for (int row = 0; row < 2; row++)
                for (int k = 0; k < cols; k++) {
                    REAL x = left + k * (w + gap), y = 0.31f + row * 0.135f;
                    AddRoundRect(h, b, x, y, x + w, y + 0.09f, detail ? 0.015f : 0.0f);
                }
            AddRoundRect(h, b, 0.28f, 0.60f, 0.72f, 0.68f, detail ? 0.03f : 0.0f);  // space bar
            break;
        }
        case Kind::Gamepad: {
            // One smooth outline with two grips.
            auto X = [&](REAL u) { return b.X(u); };
            auto Y = [&](REAL v) { return b.Y(v); };
            p.StartFigure();
            p.AddLine(X(0.29f), Y(0.21f), X(0.71f), Y(0.21f));
            p.AddBezier(X(0.71f), Y(0.21f), X(0.86f), Y(0.21f), X(0.93f), Y(0.33f), X(0.98f), Y(0.62f));
            p.AddBezier(X(0.98f), Y(0.62f), X(1.01f), Y(0.80f), X(0.87f), Y(0.86f), X(0.78f), Y(0.76f));
            p.AddBezier(X(0.78f), Y(0.76f), X(0.73f), Y(0.70f), X(0.70f), Y(0.63f), X(0.62f), Y(0.63f));
            p.AddLine(X(0.62f), Y(0.63f), X(0.38f), Y(0.63f));
            p.AddBezier(X(0.38f), Y(0.63f), X(0.30f), Y(0.63f), X(0.27f), Y(0.70f), X(0.22f), Y(0.76f));
            p.AddBezier(X(0.22f), Y(0.76f), X(0.13f), Y(0.86f), X(-0.01f), Y(0.80f), X(0.02f), Y(0.62f));
            p.AddBezier(X(0.02f), Y(0.62f), X(0.07f), Y(0.33f), X(0.14f), Y(0.21f), X(0.29f), Y(0.21f));
            p.CloseFigure();
            AddRoundRect(h, b, 0.17f, 0.385f, 0.37f, 0.445f, 0.0f);  // d-pad
            AddRoundRect(h, b, 0.24f, 0.315f, 0.30f, 0.515f, 0.0f);
            AddEllipse(h, b, 0.67f, 0.46f, 0.05f, 0.05f);  // buttons
            AddEllipse(h, b, 0.77f, 0.36f, 0.05f, 0.05f);
            break;
        }
        case Kind::Headphones:
        case Kind::Headset: {
            bool mic = kind == Kind::Headset;
            REAL bandW = 0.09f;
            Pen band(Color(255, 255, 255, 255), b.L(bandW));
            band.SetStartCap(LineCapFlat);
            band.SetEndCap(LineCapFlat);
            REAL r = 0.38f, cy = 0.05f + bandW / 2 + r;
            body.DrawArc(&band, b.X(0.5f - r), b.Y(cy - r), b.L(r * 2), b.L(r * 2), 180, 180);
            REAL cupBottom = mic ? 0.80f : 0.93f;
            AddRoundRect(p, b, 0.04f, cy - 0.03f, 0.27f, cupBottom, 0.10f);
            AddRoundRect(p, b, 0.73f, cy - 0.03f, 0.96f, cupBottom, 0.10f);
            if (mic) {
                Pen boom(Color(255, 255, 255, 255), b.L(0.07f));
                boom.SetStartCap(LineCapRound);
                boom.SetEndCap(LineCapRound);
                body.DrawBezier(&boom, b.X(0.15f), b.Y(0.80f), b.X(0.16f), b.Y(0.95f), b.X(0.30f), b.Y(0.95f), b.X(0.46f), b.Y(0.93f));
                AddEllipse(p, b, 0.50f, 0.93f, 0.06f, 0.05f);
            }
            break;
        }
        case Kind::Earbuds:
            // A pair of stemmed earbuds.
            AddEllipse(p, b, 0.28f, 0.26f, 0.21f, 0.21f);
            AddRoundRect(p, b, 0.31f, 0.26f, 0.46f, 0.95f, 0.075f);
            AddEllipse(p, b, 0.72f, 0.26f, 0.21f, 0.21f);
            AddRoundRect(p, b, 0.54f, 0.26f, 0.69f, 0.95f, 0.075f);
            AddEllipse(h, b, 0.22f, 0.23f, 0.06f, 0.06f);  // speaker openings
            AddEllipse(h, b, 0.78f, 0.23f, 0.06f, 0.06f);
            break;
        default:
            AddEllipse(p, b, 0.5f, 0.5f, 0.42f, 0.42f);
            break;
    }
    body.FillPath(&white, &p);
    holes.FillPath(&white, &h);
}

// Small status symbols used inside the widget.
void DrawSymbol(Sym sym, const Box& b, Graphics& body, Graphics& holes) {
    SolidBrush white(Color(255, 255, 255, 255));
    GraphicsPath p(FillModeWinding), h(FillModeWinding);
    switch (sym) {
        case Sym::BudLeft:
            AddEllipse(p, b, 0.42f, 0.30f, 0.27f, 0.27f);
            AddRoundRect(p, b, 0.47f, 0.30f, 0.67f, 0.97f, 0.10f);
            AddEllipse(h, b, 0.34f, 0.27f, 0.08f, 0.08f);
            break;
        case Sym::BudRight:
            AddEllipse(p, b, 0.58f, 0.30f, 0.27f, 0.27f);
            AddRoundRect(p, b, 0.33f, 0.30f, 0.53f, 0.97f, 0.10f);
            AddEllipse(h, b, 0.66f, 0.27f, 0.08f, 0.08f);
            break;
        case Sym::Case:
            AddRoundRect(p, b, 0.06f, 0.18f, 0.94f, 0.86f, 0.25f);
            AddRoundRect(h, b, 0.00f, 0.42f, 1.00f, 0.49f, 0.0f);  // lid line
            AddEllipse(h, b, 0.50f, 0.67f, 0.06f, 0.06f);          // status light
            break;
        case Sym::Bolt: {
            PointF pts[] = { PointF(b.X(0.60f), b.Y(0.02f)), PointF(b.X(0.16f), b.Y(0.58f)), PointF(b.X(0.46f), b.Y(0.58f)),
                             PointF(b.X(0.38f), b.Y(0.98f)), PointF(b.X(0.84f), b.Y(0.40f)), PointF(b.X(0.54f), b.Y(0.40f)) };
            p.AddPolygon(pts, 6);
            break;
        }
        case Sym::Moon:
            AddEllipse(p, b, 0.48f, 0.52f, 0.42f, 0.42f);
            AddEllipse(h, b, 0.68f, 0.36f, 0.36f, 0.36f);
            break;
    }
    body.FillPath(&white, &p);
    holes.FillPath(&white, &h);
}

template <class DrawFn>
void Render(Mask& body, Mask& holes, DrawFn draw) {
    Bitmap bb(body.w, body.h, PixelFormat32bppARGB), hb(holes.w, holes.h, PixelFormat32bppARGB);
    {
        Graphics gb(&bb), gh(&hb);
        for (Graphics* g : { &gb, &gh }) {
            g->Clear(Color(0, 0, 0, 0));
            g->SetSmoothingMode(SmoothingModeAntiAlias);
            g->SetPixelOffsetMode(PixelOffsetModeHalf);
        }
        draw(gb, gh);
    }
    for (auto [bmp, m] : { std::pair<Bitmap*, Mask*>{ &bb, &body }, std::pair<Bitmap*, Mask*>{ &hb, &holes } }) {
        BitmapData bd;
        Rect r(0, 0, m->w, m->h);
        if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) continue;
        for (int y = 0; y < m->h; y++) {
            const BYTE* row = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
            for (int x = 0; x < m->w; x++) m->a[(size_t)y * m->w + x] = row[x * 4 + 3];
        }
        bmp->UnlockBits(&bd);
    }
}

// Straight-alpha BGRA canvas.
struct Canvas {
    int s;
    std::vector<BYTE> px;
    explicit Canvas(int s_) : s(s_), px((size_t)s_ * s_ * 4, 0) {}
    void Over(int x, int y, COLORREF c, BYTE a) {
        if (x < 0 || y < 0 || x >= s || y >= s || !a) return;
        BYTE* p = &px[((size_t)y * s + x) * 4];
        double sa = a / 255.0, da = p[3] / 255.0, oa = sa + da * (1 - sa);
        if (oa <= 0) return;
        auto mix = [&](BYTE src, BYTE dst) { return (BYTE)std::lround((src * sa + dst * da * (1 - sa)) / oa); };
        p[0] = mix(GetBValue(c), p[0]);
        p[1] = mix(GetGValue(c), p[1]);
        p[2] = mix(GetRValue(c), p[2]);
        p[3] = (BYTE)std::lround(oa * 255);
    }
    void Rect(int x0, int y0, int x1, int y1, COLORREF c) {  // [x0,x1) x [y0,y1)
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) Over(x, y, c, 255);
    }
};

HICON ToIcon(const Canvas& cv) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cv.s;
    bi.bmiHeader.biHeight = -cv.s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;
    memcpy(bits, cv.px.data(), cv.px.size());
    HBITMAP mask = CreateBitmap(cv.s, cv.s, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void Composite(Canvas& cv, const Mask& body, const Mask& holes, COLORREF fg) {
    for (int y = 0; y < cv.s; y++)
        for (int x = 0; x < cv.s; x++) {
            size_t i = (size_t)y * cv.s + x;
            int a = body.a[i] * (255 - holes.a[i]) / 255;
            cv.Over(x, y, fg, (BYTE)a);
        }
}

void PaintGlyph(Canvas& cv, Kind kind, COLORREF fg, int x0, int y0, int side) {
    Mask body(cv.s, cv.s), holes(cv.s, cv.s);
    Box b{ (REAL)x0, (REAL)y0, (REAL)side };
    Render(body, holes, [&](Graphics& gb, Graphics& gh) { DrawGlyph(kind, b, gb, gh); });
    Composite(cv, body, holes, fg);
}

}  // namespace

void IconsInit() {
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdipToken, &in, nullptr);
}

void IconsShutdown() {
    if (g_gdipToken) GdiplusShutdown(g_gdipToken);
    g_gdipToken = 0;
}

// How much of the ring's inner circle each symbol may use (wide shapes need a little less).
REAL GlyphScale(Kind k) {
    switch (k) {
        case Kind::Mouse: return 0.90f;
        case Kind::Gamepad: return 0.92f;
        case Kind::Keyboard: return 0.94f;
        case Kind::Earbuds: return 0.86f;
        case Kind::Headphones:
        case Kind::Headset: return 0.80f;
        default: return 0.80f;
    }
}

// Device symbol inside a ring. The ring is the battery: a dim full circle, and an arc from 12
// o'clock clockwise that covers `pct` of it (-1 = unknown: track only).
HICON MakeDeviceIcon(Kind kind, int pct, COLORREF fg, COLORREF fill, int s) {
    Canvas cv(s);
    REAL t = s >= 32 ? 3.0f : (s >= 20 ? 2.0f : 1.5f);  // ring thickness
    REAL r = s / 2.0f - t / 2.0f;                        // ring centre line radius
    REAL c = s / 2.0f;
    Mask track(s, s), arc(s, s);
    {
        Bitmap tb(s, s, PixelFormat32bppARGB), ab(s, s, PixelFormat32bppARGB);
        Graphics gt(&tb), ga(&ab);
        for (Graphics* g : { &gt, &ga }) {
            g->Clear(Color(0, 0, 0, 0));
            g->SetSmoothingMode(SmoothingModeAntiAlias);
            g->SetPixelOffsetMode(PixelOffsetModeHalf);
        }
        Pen pen(Color(255, 255, 255, 255), t);
        gt.DrawEllipse(&pen, c - r, c - r, 2 * r, 2 * r);
        if (pct >= 100) {
            ga.DrawEllipse(&pen, c - r, c - r, 2 * r, 2 * r);
        } else if (pct > 0) {
            REAL sweep = std::max(360.0f * pct / 100.0f, 12.0f);  // a sliver stays visible at 1-3 %
            ga.DrawArc(&pen, c - r, c - r, 2 * r, 2 * r, -90.0f, sweep);
        }
        for (auto [bmp, m] : { std::pair<Bitmap*, Mask*>{ &tb, &track }, std::pair<Bitmap*, Mask*>{ &ab, &arc } }) {
            BitmapData bd;
            Rect rc(0, 0, s, s);
            if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) continue;
            for (int y = 0; y < s; y++) {
                const BYTE* row = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
                for (int x = 0; x < s; x++) m->a[(size_t)y * s + x] = row[x * 4 + 3];
            }
            bmp->UnlockBits(&bd);
        }
    }
    for (int i = 0; i < s * s; i++) {
        int x = i % s, y = i / s;
        cv.Over(x, y, fg, (BYTE)(track.a[i] * 70 / 255));  // dim track
        cv.Over(x, y, fill, arc.a[i]);                      // level
    }
    // Symbol centred inside the ring, leaving a 1 px gap.
    REAL inner = s - 2 * t - 2.0f;
    int side = (int)std::lround(inner * GlyphScale(kind));
    int off = (s - side) / 2;
    if (side > 4) PaintGlyph(cv, kind, fg, off, off, side);
    return ToIcon(cv);
}

// Just the symbol (used by the widget).
HICON MakeGlyphIcon(Kind kind, COLORREF fg, int s) {
    Canvas cv(s);
    PaintGlyph(cv, kind, fg, 0, 0, s);
    return ToIcon(cv);
}

// A status symbol (left bud, right bud, case, charging bolt, asleep moon).
HICON MakeSymbolIcon(Sym sym, COLORREF fg, int s) {
    Canvas cv(s);
    Mask body(s, s), holes(s, s);
    Box b{ 0, 0, (REAL)s };
    Render(body, holes, [&](Graphics& gb, Graphics& gh) { DrawSymbol(sym, b, gb, gh); });
    Composite(cv, body, holes, fg);
    return ToIcon(cv);
}

// A plain number, for the "number only" icon style.
HICON MakeNumberIcon(const std::wstring& text, COLORREF color, int size) {
    Canvas cv(size);
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        FontFamily fam(L"Segoe UI");
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        REAL em = text.size() >= 3 ? size * 0.62f : size * 0.86f;
        SolidBrush br(Color(255, 255, 255, 255));
        for (; em > 6; em -= 0.5f) {
            Font f(&fam, em, FontStyleBold, UnitPixel);
            RectF bounds;
            g.MeasureString(text.c_str(), -1, &f, PointF(0, 0), &bounds);
            if (bounds.Width - em * 0.25f <= size + 1) {
                g.DrawString(text.c_str(), -1, &f, RectF(-size * 0.5f, 0, size * 2.0f, (REAL)size), &fmt, &br);
                break;
            }
        }
    }
    BitmapData bd;
    Rect r(0, 0, size, size);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int y = 0; y < size; y++) {
            const BYTE* row = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
            for (int x = 0; x < size; x++) cv.Over(x, y, color, row[x * 4 + 3]);
        }
        bmp.UnlockBits(&bd);
    }
    return ToIcon(cv);
}
