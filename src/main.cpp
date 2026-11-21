// Juice: tray icons with the battery % of each device, a desktop widget, low-battery alerts.
#include "common.h"

#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>
#include <set>
#include <vector>

namespace {

// ---- constants ----------------------------------------------------------------------------------

const wchar_t kAppName[] = L"Juice";
const wchar_t kMsgClass[] = L"JuiceMsgWindow";
const wchar_t kWidgetClass[] = L"JuiceWidget";
const wchar_t kMutexName[] = L"Local\\JuiceBatteryTray-Anas";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,
    WM_APP_POLL = WM_APP + 2,
    WM_APP_SHOW = WM_APP + 3,
    WM_APP_EXIT = WM_APP + 4,   // "Juice.exe --exit" (Minibar's Stop / updates): close cleanly
};
enum : UINT_PTR {
    TIMER_HOUSEKEEPING = 1,  // every 20 s: pending alerts, stale devices, watchdog
    TIMER_DEVCHANGE = 2,
    TIMER_RESUME = 3,
    TIMER_PROMOTE = 4,
    TIMER_SAVEPOS = 5,
};
enum : UINT {
    IDM_WIDGET = 100,
    IDM_ONTOP,
    IDM_AUTOSTART,
    IDM_REFRESH,
    IDM_DIAG,
    IDM_EXIT,
    IDM_STYLE_SYMBOL,
    IDM_STYLE_NUMBER,
    IDM_ALERT_OFF = 200,  // 200 + percent
};
const UINT kPlaceholderIcon = 1;

// ---- settings -----------------------------------------------------------------------------------

std::wstring IniPath() { return DataDir() + L"\\settings.ini"; }
int GetInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(kAppName, key, def, IniPath().c_str()); }
void SetInt(const wchar_t* key, int v) { WritePrivateProfileStringW(kAppName, key, std::to_wstring(v).c_str(), IniPath().c_str()); }

struct Settings {
    int alertAt = 20;  // 0 = off
    bool widget = true;
    bool onTop = false;
    int wx = INT_MIN, wy = INT_MIN;
    bool autostart = true;
    int iconStyle = 0;  // 0 = device symbol + ring, 1 = number
    void Load() {
        alertAt = GetInt(L"AlertAt", 20);
        widget = GetInt(L"Widget", 1) != 0;
        onTop = GetInt(L"WidgetOnTop", 0) != 0;
        wx = GetInt(L"WidgetX", INT_MIN);
        wy = GetInt(L"WidgetY", INT_MIN);
        autostart = GetInt(L"Autostart", 1) != 0;
        iconStyle = GetInt(L"IconStyle", 0);
    }
} g_set;

// ---- state --------------------------------------------------------------------------------------

struct Entry {
    Reading r;
    ULONGLONG lastSeen = 0;
    bool stale = false;
    bool iconAdded = false;
    UINT iconId = 0;
    HICON icon = nullptr;
    std::wstring iconSig, tipSig;
    bool alerted = false, alertPending = false;
};

HINSTANCE g_inst;
HWND g_msgWnd = nullptr, g_widget = nullptr;
UINT g_taskbarCreated = 0;
std::map<std::wstring, Entry> g_devs;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;
bool g_firstRun = false;
ULONGLONG g_startTick = 0;  // AwakeMs()
ULONGLONG g_resumeAt = 0;   // AwakeMs() of the last wake from sleep

// ---- helpers ------------------------------------------------------------------------------------

UINT IconIdFor(const std::wstring& key) {
    // Stable across runs, so Windows remembers per icon whether it is shown on the taskbar.
    UINT h = 2166136261u;
    for (wchar_t c : key) {
        h ^= (UINT)c;
        h *= 16777619u;
    }
    return 1000 + h % 60000;
}

int KindOrder(Kind k) {
    switch (k) {
        case Kind::Earbuds: return 0;
        case Kind::Headphones: return 1;
        case Kind::Headset: return 2;
        case Kind::Mouse: return 3;
        case Kind::Keyboard: return 4;
        case Kind::Gamepad: return 5;
        default: return 6;
    }
}

std::vector<Entry*> Ordered() {
    std::vector<Entry*> v;
    for (auto& [k, e] : g_devs) v.push_back(&e);
    std::sort(v.begin(), v.end(), [](Entry* a, Entry* b) {
        if (KindOrder(a->r.kind) != KindOrder(b->r.kind)) return KindOrder(a->r.kind) < KindOrder(b->r.kind);
        return a->r.name < b->r.name;
    });
    return v;
}

bool LightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

enum class Tone { Normal, Warn, Low, Charging, Stale };

Tone ToneFor(const Entry& e) {
    if (e.stale) return Tone::Stale;
    if (e.r.charging) return Tone::Charging;
    int t = g_set.alertAt > 0 ? g_set.alertAt : 20;
    if (e.r.percent >= 0 && e.r.percent <= t) return Tone::Low;
    if (e.r.percent >= 0 && e.r.percent <= t + 15) return Tone::Warn;
    return Tone::Normal;
}

COLORREF ToneColor(Tone t, bool light) {
    switch (t) {
        case Tone::Low: return light ? RGB(0xC4, 0x2B, 0x1C) : RGB(0xFF, 0x63, 0x5A);
        case Tone::Warn: return light ? RGB(0xB3, 0x6B, 0x00) : RGB(0xFF, 0xC1, 0x3B);
        case Tone::Charging: return light ? RGB(0x0F, 0x7B, 0x0F) : RGB(0x6C, 0xD8, 0x6C);
        case Tone::Stale: return light ? RGB(0x8A, 0x8A, 0x8A) : RGB(0x8C, 0x8C, 0x8C);
        default: return light ? RGB(0x1A, 0x1A, 0x1A) : RGB(0xFF, 0xFF, 0xFF);
    }
}

std::wstring PctText(int v) { return v < 0 ? L"?" : std::to_wstring(v) + L"%"; }

// ---- tray icon drawing (icons.cpp) ----------------------------------------------------------------

int TrayIconSize() {
    int s = GetSystemMetrics(SM_CXSMICON);
    return s > 0 ? s : 16;
}

// ---- tray icons -----------------------------------------------------------------------------------

std::wstring TipFor(const Entry& e) {
    std::wstring t = e.r.name + L"\n";
    if (e.r.buds) {
        t += L"L " + PctText(e.r.left) + (e.r.leftChg ? L"\x26A1" : L"") + L"   R " + PctText(e.r.right) + (e.r.rightChg ? L"\x26A1" : L"") +
             L"   Case " + PctText(e.r.box) + (e.r.boxChg ? L"\x26A1" : L"");
    } else {
        t += PctText(e.r.percent);
        if (e.r.charging) t += L" \x26A1 charging";
    }
    if (e.stale) {
        ULONGLONG mins = (GetTickCount64() - e.lastSeen) / 60000;
        t += mins ? Fmt(L"\n(asleep, last seen %llu min ago)", mins) : std::wstring(L"\n(asleep)");
    }
    if (t.size() > 127) t.resize(127);
    return t;
}

void FillNid(NOTIFYICONDATAW& nid, UINT id) {
    nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = id;
}

bool AddIcon(UINT id, HICON icon, const std::wstring& tip) {
    NOTIFYICONDATAW nid;
    FillNid(nid, id);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon;
    wcsncpy(nid.szTip, tip.c_str(), 127);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        // Already there (e.g. after an Explorer hiccup): update it instead.
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void ModifyIcon(UINT id, HICON icon, const std::wstring& tip) {
    NOTIFYICONDATAW nid;
    FillNid(nid, id);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = icon;
    wcsncpy(nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void DeleteIcon(UINT id) {
    NOTIFYICONDATAW nid;
    FillNid(nid, id);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void RefreshIcons(bool force) {
    bool light = LightTaskbar();
    int size = TrayIconSize();
    auto list = Ordered();
    bool anyAdded = false;
    for (Entry* e : list) {
        std::wstring text = e->r.percent >= 0 ? std::to_wstring(e->r.percent) : L"?";
        Tone tone = ToneFor(*e);
        COLORREF col = ToneColor(tone, light);
        // Symbol style: the symbol in the taskbar's text colour (grey when asleep), the ring
        // filled in the level's colour (white normally, amber / red when low, green charging).
        COLORREF fg = e->stale ? ToneColor(Tone::Stale, light) : ToneColor(Tone::Normal, light);
        std::wstring sig = text + Fmt(L"|%06X|%d|%d|%d|%d", (unsigned)col, size, g_set.iconStyle, (int)e->r.kind, e->stale);
        std::wstring tip = TipFor(*e);
        if (!force && e->iconAdded && sig == e->iconSig && tip == e->tipSig) continue;
        if (sig != e->iconSig || !e->icon || force) {
            HICON fresh = g_set.iconStyle == 1 ? MakeNumberIcon(text, col, size) : MakeDeviceIcon(e->r.kind, e->r.percent, fg, col, size);
            if (fresh) {
                if (e->icon) DestroyIcon(e->icon);
                e->icon = fresh;
                e->iconSig = sig;
            }
        }
        e->tipSig = tip;
        e->iconId = IconIdFor(e->r.key);
        if (!e->iconAdded || force) {
            e->iconAdded = AddIcon(e->iconId, e->icon, tip);
            anyAdded |= e->iconAdded;
        } else {
            ModifyIcon(e->iconId, e->icon, tip);
        }
    }
    // A grey dash while no device is found, so the menu stays reachable.
    bool wantPlaceholder = list.empty();
    if (wantPlaceholder && (!g_placeholder || force)) {
        if (g_placeholderIcon) DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = MakeNumberIcon(L"\x2013", ToneColor(Tone::Stale, light), size);
        g_placeholder = AddIcon(kPlaceholderIcon, g_placeholderIcon, L"Juice\nLooking for your devices...");
        anyAdded |= g_placeholder;
    } else if (!wantPlaceholder && g_placeholder) {
        DeleteIcon(kPlaceholderIcon);
        g_placeholder = false;
    }
    if (anyAdded) SetTimer(g_msgWnd, TIMER_PROMOTE, 3000, nullptr);
}

// Explorer stores paths under a known folder as "{folder GUID}\\rest", e.g. Documents.
std::wstring ExpandKnownFolder(const std::wstring& p) {
    if (p.size() < 39 || p[0] != L'{' || p[37] != L'}') return p;
    GUID g;
    if (FAILED(CLSIDFromString(p.substr(0, 38).c_str(), &g))) return p;
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(g, 0, nullptr, &base)) || !base) return p;
    std::wstring out = std::wstring(base) + p.substr(38);
    CoTaskMemFree(base);
    return out;
}

// Windows 11 puts new tray icons in the overflow menu. Show ours on the taskbar the first time
// each one appears; if Anas hides one later, Windows records that and it is left alone.
void PromoteIcons() {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    std::wstring exe = ExePath();
    for (DWORD i = 0;; i++) {
        wchar_t sub[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD sz = sizeof(path) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(k, L"ExecutablePath", nullptr, &type, (BYTE*)path, &sz) == ERROR_SUCCESS && type == REG_SZ &&
            _wcsicmp(ExpandKnownFolder(path).c_str(), exe.c_str()) == 0) {
            DWORD v = 0, vsz = sizeof(v);
            if (RegQueryValueExW(k, L"IsPromoted", nullptr, nullptr, (BYTE*)&v, &vsz) != ERROR_SUCCESS) {
                DWORD one = 1;
                RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
            }
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
}

// ---- alerts ---------------------------------------------------------------------------------------

bool UserBusy() {
    QUERY_USER_NOTIFICATION_STATE s;
    if (FAILED(SHQueryUserNotificationState(&s))) return false;
    return s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE;
}

void ShowAlert(Entry& e) {
    NOTIFYICONDATAW nid;
    FillNid(nid, e.iconId);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING | NIIF_LARGE_ICON;
    std::wstring title = e.r.name + L" is low";
    std::wstring body = e.r.buds ? (L"L " + PctText(e.r.left) + L"  R " + PctText(e.r.right) + L"  Case " + PctText(e.r.box))
                                 : (PctText(e.r.percent) + L" left. Time to charge.");
    wcsncpy(nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(nid.szInfo, body.c_str(), 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    Log(L"alert: %ls %d%%", e.r.name.c_str(), e.r.percent);
}

void CheckAlerts() {
    if (g_set.alertAt <= 0) return;
    for (auto& [k, e] : g_devs) {
        int pct = e.r.percent;
        if (e.r.charging || pct >= g_set.alertAt + 5) {
            e.alerted = false;
            e.alertPending = false;
            continue;
        }
        if (e.stale || pct < 0 || pct > g_set.alertAt || e.alerted || !e.iconAdded) continue;
        if (UserBusy()) {
            e.alertPending = true;  // gaming / full screen: tell later
            continue;
        }
        ShowAlert(e);
        e.alerted = true;
        e.alertPending = false;
    }
}

// ---- autostart ------------------------------------------------------------------------------------

bool AutostartActive() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    BYTE approved[12] = {};
    DWORD asz = sizeof(approved);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kAppName, RRF_RT_REG_BINARY, nullptr, approved, &asz) == ERROR_SUCCESS && asz >= 1 &&
        (approved[0] & 1))
        return false;  // switched off in Task Manager > Startup apps
    return true;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --autostart";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
    if (on) {
        // Re-enable if it was switched off in Task Manager.
        HKEY a;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &a) == ERROR_SUCCESS) {
            RegDeleteValueW(a, kAppName);
            RegCloseKey(a);
        }
    }
}

// ---- restart after a crash / hang -----------------------------------------------------------------

bool MayRestart() {
    // At most 3 automatic restarts per 10 minutes, so a broken setup cannot loop forever.
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long long now = (long long)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL);
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(kAppName, L"Restarts", L"", buf, 256, IniPath().c_str());
    std::vector<long long> times;
    for (wchar_t* p = buf; *p;) {
        wchar_t* end;
        long long v = wcstoll(p, &end, 10);
        if (end == p) break;
        if (now - v < 600) times.push_back(v);
        p = *end ? end + 1 : end;
    }
    if (times.size() >= 3) return false;
    times.push_back(now);
    std::wstring s;
    for (auto t : times) s += std::to_wstring(t) + L",";
    WritePrivateProfileStringW(kAppName, L"Restarts", s.c_str(), IniPath().c_str());
    return true;
}

void RelaunchSelf() {
    if (!MayRestart()) {
        Log(L"not restarting: too many restarts in 10 minutes");
        return;
    }
    std::wstring cmd = L"\"" + ExePath() + L"\" --restarted";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    Log(L"crash: exception %08lX at %p - restarting", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
        ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    for (auto& [k, e] : g_devs)
        if (e.iconAdded) DeleteIcon(e.iconId);
    if (g_placeholder) DeleteIcon(kPlaceholderIcon);
    RelaunchSelf();
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- widget ---------------------------------------------------------------------------------------

struct WidgetFonts {
    HFONT name = nullptr, pct = nullptr, small = nullptr, glyph = nullptr, bolt = nullptr;
    int dpi = 0;
} g_wf;

int Sc(int v, int dpi) { return MulDiv(v, dpi, 96); }

void EnsureFonts(int dpi) {
    if (g_wf.dpi == dpi && g_wf.name) return;
    for (HFONT* f : { &g_wf.name, &g_wf.pct, &g_wf.small, &g_wf.glyph, &g_wf.bolt })
        if (*f) DeleteObject(*f);
    auto mk = [&](int px, int weight, const wchar_t* face) {
        return CreateFontW(-Sc(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_wf.name = mk(13, FW_NORMAL, L"Segoe UI");
    g_wf.pct = mk(14, FW_SEMIBOLD, L"Segoe UI");
    g_wf.small = mk(11, FW_NORMAL, L"Segoe UI");
    // Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10 (same code points).
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Segoe Fluent Icons");
    HDC dc = GetDC(nullptr);
    bool fluent = false;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *(bool*)p = true; return 0; },
                        (LPARAM)&fluent, 0);
    ReleaseDC(nullptr, dc);
    g_wf.glyph = mk(16, FW_NORMAL, fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
    g_wf.bolt = mk(12, FW_NORMAL, L"Segoe UI Symbol");
    g_wf.dpi = dpi;
}

HICON WidgetGlyph(Kind k, COLORREF c, int size) {
    static std::map<std::wstring, HICON> cache;
    std::wstring key = Fmt(L"%d|%06X|%d", (int)k, (unsigned)c, size);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HICON i = MakeGlyphIcon(k, c, size);
    cache[key] = i;
    return i;
}

int RowHeight(const Entry& e, int dpi) { return Sc(e.r.buds || e.stale ? 54 : 40, dpi); }

SIZE WidgetSize(int dpi) {
    int h = Sc(8, dpi) * 2;
    auto list = Ordered();
    if (list.empty()) h += Sc(40, dpi);
    for (Entry* e : list) h += RowHeight(*e, dpi);
    return { Sc(270, dpi), h };
}

void PaintWidget(HWND hwnd, HDC target) {
    int dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    EnsureFonts(dpi);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    int pad = Sc(12, dpi);
    int y = Sc(8, dpi);
    auto list = Ordered();
    if (list.empty()) {
        SelectObject(dc, g_wf.name);
        SetTextColor(dc, RGB(0xA0, 0xA0, 0xA0));
        RECT t{ pad, y, rc.right - pad, y + Sc(40, dpi) };
        DrawTextW(dc, L"Looking for your devices...\nTurn one on and it shows up here.", -1, &t, DT_LEFT | DT_WORDBREAK);
    }
    for (Entry* e : list) {
        int rowH = RowHeight(*e, dpi);
        Tone tone = ToneFor(*e);
        COLORREF accent = ToneColor(tone, false);
        // Device symbol (same drawing as the tray icon)
        int gs = Sc(22, dpi);
        HICON gi = WidgetGlyph(e->r.kind, e->stale ? RGB(0x70, 0x70, 0x70) : RGB(0xE0, 0xE0, 0xE0), gs);
        if (gi) DrawIconEx(dc, pad, y + Sc(4, dpi), gi, gs, gs, 0, nullptr, DI_NORMAL);
        int x0 = pad + Sc(32, dpi);
        // Percent (right) with a bolt while charging
        std::wstring pct = PctText(e->r.percent);
        SelectObject(dc, g_wf.pct);
        SetTextColor(dc, tone == Tone::Normal ? RGB(0xFF, 0xFF, 0xFF) : accent);
        RECT p{ x0, y + Sc(4, dpi), rc.right - pad, y + Sc(24, dpi) };
        DrawTextW(dc, pct.c_str(), -1, &p, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SIZE pctSz{};
        GetTextExtentPoint32W(dc, pct.c_str(), (int)pct.size(), &pctSz);
        int nameRight = rc.right - pad - pctSz.cx - Sc(6, dpi);
        if (e->r.charging) {
            SelectObject(dc, g_wf.bolt);
            SetTextColor(dc, ToneColor(Tone::Charging, false));
            RECT b{ x0, y + Sc(4, dpi), nameRight, y + Sc(24, dpi) };
            DrawTextW(dc, L"\x26A1", -1, &b, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            nameRight -= Sc(14, dpi);
        }
        // Name
        SelectObject(dc, g_wf.name);
        SetTextColor(dc, e->stale ? RGB(0x90, 0x90, 0x90) : RGB(0xF0, 0xF0, 0xF0));
        RECT n{ x0, y + Sc(4, dpi), nameRight, y + Sc(24, dpi) };
        DrawTextW(dc, e->r.name.c_str(), -1, &n, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        // Level bar
        int barY = y + Sc(28, dpi), barH = Sc(4, dpi);
        RECT track{ x0, barY, rc.right - pad, barY + barH };
        HBRUSH tb = CreateSolidBrush(RGB(0x3A, 0x3A, 0x3A));
        FillRect(dc, &track, tb);
        DeleteObject(tb);
        if (e->r.percent >= 0) {
            RECT fill = track;
            fill.right = track.left + (track.right - track.left) * e->r.percent / 100;
            HBRUSH fb = CreateSolidBrush(tone == Tone::Normal ? RGB(0xC8, 0xC8, 0xC8) : accent);
            FillRect(dc, &fill, fb);
            DeleteObject(fb);
        }
        // Earbuds: left / right / case
        if (e->r.buds) {
            SelectObject(dc, g_wf.small);
            SetTextColor(dc, RGB(0xB0, 0xB0, 0xB0));
            auto part = [](const wchar_t* label, int v, bool c) {
                return std::wstring(label) + L" " + PctText(v) + (c ? L" \x26A1" : L"");
            };
            std::wstring s = part(L"L", e->r.left, e->r.leftChg) + L"     " + part(L"R", e->r.right, e->r.rightChg) + L"     " +
                             part(L"Case", e->r.box, e->r.boxChg);
            if (e->stale) s += L"   (last seen)";
            RECT t{ x0, barY + Sc(8, dpi), rc.right - pad, barY + Sc(24, dpi) };
            DrawTextW(dc, s.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (e->stale) {
            SelectObject(dc, g_wf.small);
            SetTextColor(dc, RGB(0x80, 0x80, 0x80));
            RECT t{ x0, barY + Sc(8, dpi), rc.right - pad, barY + Sc(24, dpi) };
            DrawTextW(dc, L"asleep - last known level", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        y += rowH;
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void PlaceWidget(bool resetPos, bool zorder = false) {
    if (!g_widget) return;
    int dpi = GetDpiForWindow(g_widget);
    if (!dpi) dpi = 96;
    SIZE sz = WidgetSize(dpi);
    RECT wr;
    GetWindowRect(g_widget, &wr);
    int x = wr.left, y = wr.top;
    if (resetPos) {
        x = g_set.wx;
        y = g_set.wy;
        POINT pt{ x + 20, y + 20 };
        if (x == INT_MIN || !MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) {
            RECT work;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            x = work.right - sz.cx - Sc(16, dpi);
            y = work.bottom - sz.cy - Sc(16, dpi);
        }
    }
    // Growing (more devices) must not push the widget off the bottom of its screen.
    {
        POINT pt{ x + 20, y + 20 };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (mon && GetMonitorInfoW(mon, &mi) && y + sz.cy > mi.rcWork.bottom)
            y = std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - (int)sz.cy);
    }
    // Only touch the z-order when asked: re-asserting HWND_NOTOPMOST on every refresh would pull
    // the widget above the window Anas is working in.
    UINT flags = SWP_NOACTIVATE | (zorder ? 0 : SWP_NOZORDER);
    SetWindowPos(g_widget, g_set.onTop ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, sz.cx, sz.cy, flags);
    InvalidateRect(g_widget, nullptr, FALSE);
}

void ShowWidget(bool show) {
    if (!g_widget) return;
    if (show) {
        PlaceWidget(false);
        ShowWindow(g_widget, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_widget, SW_HIDE);
    }
}

// ---- menu -----------------------------------------------------------------------------------------

void ShowMenu(HWND owner) {
    HMENU m = CreatePopupMenu();
    HMENU alert = CreatePopupMenu();
    AppendMenuW(alert, MF_STRING | (g_set.alertAt == 0 ? MF_CHECKED : 0), IDM_ALERT_OFF, L"Off");
    for (int v : { 10, 15, 20, 25, 30 })
        AppendMenuW(alert, MF_STRING | (g_set.alertAt == v ? MF_CHECKED : 0), IDM_ALERT_OFF + v, (std::to_wstring(v) + L"%").c_str());
    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING | (g_set.iconStyle == 0 ? MF_CHECKED : 0), IDM_STYLE_SYMBOL, L"Device symbol + ring");
    AppendMenuW(style, MF_STRING | (g_set.iconStyle == 1 ? MF_CHECKED : 0), IDM_STYLE_NUMBER, L"Number");
    AppendMenuW(m, MF_STRING | (g_set.widget ? MF_CHECKED : 0), IDM_WIDGET, L"Show widget");
    AppendMenuW(m, MF_STRING | (g_set.onTop ? MF_CHECKED : 0), IDM_ONTOP, L"Keep widget on top");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)style, L"Taskbar icons");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)alert, L"Low-battery alert");
    AppendMenuW(m, MF_STRING | (AutostartActive() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"Refresh now");
    AppendMenuW(m, MF_STRING, IDM_DIAG, L"Diagnostics...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd == IDM_WIDGET) {
        g_set.widget = !g_set.widget;
        SetInt(L"Widget", g_set.widget);
        ShowWidget(g_set.widget);
    } else if (cmd == IDM_ONTOP) {
        g_set.onTop = !g_set.onTop;
        SetInt(L"WidgetOnTop", g_set.onTop);
        PlaceWidget(false, true);
    } else if (cmd == IDM_AUTOSTART) {
        bool on = !AutostartActive();
        g_set.autostart = on;
        SetInt(L"Autostart", on);
        SetAutostart(on);
    } else if (cmd == IDM_STYLE_SYMBOL || cmd == IDM_STYLE_NUMBER) {
        g_set.iconStyle = cmd == IDM_STYLE_NUMBER ? 1 : 0;
        SetInt(L"IconStyle", g_set.iconStyle);
        RefreshIcons(true);
    } else if (cmd == IDM_REFRESH) {
        RequestPoll();
    } else if (cmd == IDM_DIAG) {
        RequestDiagnostics();
    } else if (cmd == IDM_EXIT) {
        DestroyWindow(g_msgWnd);
    } else if (cmd >= IDM_ALERT_OFF && cmd <= IDM_ALERT_OFF + 100) {
        g_set.alertAt = (int)(cmd - IDM_ALERT_OFF);
        SetInt(L"AlertAt", g_set.alertAt);
        for (auto& [k, e] : g_devs) e.alerted = false;
        RefreshIcons(true);
        CheckAlerts();
        if (g_widget) InvalidateRect(g_widget, nullptr, FALSE);
    }
}

// ---- readings -------------------------------------------------------------------------------------

void ApplyReadings(std::vector<Reading>& readings) {
    ULONGLONG now = GetTickCount64();
    std::set<std::wstring> seen;
    for (Reading& r : readings) {
        seen.insert(r.key);
        Entry& e = g_devs[r.key];
        int oldPct = e.r.percent;
        bool isNew = e.lastSeen == 0;
        e.r = r;
        if (e.r.percent < 0 && oldPct >= 0) e.r.percent = oldPct;  // e.g. charging without a level: keep the last one
        e.stale = r.asleep;
        if (!r.asleep) e.lastSeen = now;
        else if (isNew) e.lastSeen = now;
        if (isNew) Log(L"found %ls (%ls): %d%%", r.name.c_str(), r.via.c_str(), r.percent);
    }
    for (auto it = g_devs.begin(); it != g_devs.end();) {
        Entry& e = it->second;
        if (!seen.count(it->first)) {
            e.stale = true;
            if (now - e.lastSeen > (ULONGLONG)e.r.graceSec * 1000) {
                Log(L"gone: %ls", e.r.name.c_str());
                if (e.iconAdded) DeleteIcon(e.iconId);
                if (e.icon) DestroyIcon(e.icon);
                it = g_devs.erase(it);
                continue;
            }
        }
        ++it;
    }
    RefreshIcons(false);
    CheckAlerts();
    if (g_widget && IsWindowVisible(g_widget)) PlaceWidget(false);
}

void WriteDiagnostics(const std::wstring& text) {
    std::wstring path = DataDir() + L"\\diagnostics.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), bytes, nullptr, nullptr);
    DWORD w;
    WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    CloseHandle(h);
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- window procedures ------------------------------------------------------------------------------

LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            return hit == HTCLIENT ? HTCAPTION : hit;  // drag from anywhere
        }
        case WM_NCRBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu(g_msgWnd);
            return 0;
        case WM_NCLBUTTONDBLCLK:
            RequestPoll();
            return 0;
        case WM_EXITSIZEMOVE:
            SetTimer(g_msgWnd, TIMER_SAVEPOS, 300, nullptr);
            return 0;
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            PlaceWidget(false);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintWidget(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            g_set.widget = false;
            SetInt(L"Widget", 0);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        // Explorer restarted: every icon has to be added again.
        for (auto& [k, e] : g_devs) e.iconAdded = false;
        g_placeholder = false;
        RefreshIcons(true);
        return 0;
    }
    switch (msg) {
        case WM_APP_POLL: {
            if (wp == 1) {
                std::unique_ptr<std::wstring> text((std::wstring*)lp);
                WriteDiagnostics(*text);
            } else {
                std::unique_ptr<std::vector<Reading>> r((std::vector<Reading>*)lp);
                ApplyReadings(*r);
            }
            return 0;
        }
        case WM_APP_TRAY: {
            // NOTIFYICON_VERSION_4 also sends the raw mouse messages; only these two events count,
            // otherwise every click would act twice.
            UINT ev = LOWORD(lp);
            if (ev == WM_CONTEXTMENU) ShowMenu(hwnd);
            else if (ev == NIN_SELECT || ev == NIN_KEYSELECT) {
                g_set.widget = !(g_widget && IsWindowVisible(g_widget));
                SetInt(L"Widget", g_set.widget);
                ShowWidget(g_set.widget);
            }
            return 0;
        }
        case WM_APP_EXIT:
            DestroyWindow(hwnd);   // same as Exit in the menu: icons removed, poller stopped
            return 0;
        case WM_APP_SHOW:
            g_set.widget = true;
            SetInt(L"Widget", 1);
            ShowWidget(true);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_HOUSEKEEPING) {
                // Age out quiet devices without waiting for the next poll result.
                ULONGLONG now = GetTickCount64();
                bool changed = false;
                for (auto& [k, e] : g_devs)
                    if (!e.stale && now - e.lastSeen > 100 * 1000) {
                        e.stale = true;
                        changed = true;
                    }
                if (changed) RefreshIcons(false);
                CheckAlerts();  // delivers alerts held back during games
                RefreshIcons(false);  // tooltips show "last seen" minutes
                ULONGLONG awake = AwakeMs(), hb = PollerHeartbeat();
                auto olderThan5Min = [&](ULONGLONG t) { return t < awake && awake - t > 5 * 60 * 1000; };
                if (olderThan5Min(hb) && olderThan5Min(g_startTick) && (g_resumeAt == 0 || olderThan5Min(g_resumeAt))) {
                    Log(L"watchdog: poller stuck for over 5 minutes - restarting");
                    RelaunchSelf();
                    for (auto& [k2, e2] : g_devs)
                        if (e2.iconAdded) DeleteIcon(e2.iconId);
                    ExitProcess(2);
                }
            } else if (wp == TIMER_DEVCHANGE || wp == TIMER_RESUME) {
                KillTimer(hwnd, wp);
                RequestPoll();
            } else if (wp == TIMER_PROMOTE) {
                KillTimer(hwnd, wp);
                PromoteIcons();
            } else if (wp == TIMER_SAVEPOS) {
                KillTimer(hwnd, wp);
                if (g_widget) {
                    RECT r;
                    GetWindowRect(g_widget, &r);
                    g_set.wx = r.left;
                    g_set.wy = r.top;
                    SetInt(L"WidgetX", r.left);
                    SetInt(L"WidgetY", r.top);
                }
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
                SetTimer(hwnd, TIMER_DEVCHANGE, 3000, nullptr);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                g_resumeAt = AwakeMs();
                SetTimer(hwnd, TIMER_RESUME, 8000, nullptr);
            }
            return TRUE;
        case WM_SETTINGCHANGE:
            if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) RefreshIcons(true);
            return 0;
        case WM_DISPLAYCHANGE:
            RefreshIcons(true);
            if (g_widget) PlaceWidget(false);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_HOUSEKEEPING);
            for (auto& [k, e] : g_devs)
                if (e.iconAdded) DeleteIcon(e.iconId);
            if (g_placeholder) DeleteIcon(kPlaceholderIcon);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnableDpiAwareness() {
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = inst;
    g_startTick = AwakeMs();
    SetUnhandledExceptionFilter(CrashFilter);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    EnableDpiAwareness();
    bool restarted = cmdLine && wcsstr(cmdLine, L"--restarted");
    if (cmdLine && wcsstr(cmdLine, L"--exit")) {
        // asked to close a running Juice; never starts one
        HWND other = FindWindowW(kMsgClass, nullptr);
        if (other) PostMessageW(other, WM_APP_EXIT, 0, 0);
        return 0;
    }

    // One copy only. A restart waits for the old copy to finish exiting.
    HANDLE mutex = nullptr;
    for (int i = 0; i < (restarted ? 50 : 1); i++) {
        mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(mutex);
        mutex = nullptr;
        if (restarted) Sleep(200);
    }
    if (!mutex) {
        HWND other = FindWindowW(kMsgClass, nullptr);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IconsInit();
    g_firstRun = GetInt(L"FirstRunDone", 0) == 0;
    g_set.Load();
    if (g_firstRun) SetInt(L"FirstRunDone", 1);
    if (g_set.autostart && (g_firstRun || AutostartActive())) SetAutostart(true);  // keeps the path current if the app moved
    Log(L"Juice started%ls (data in %ls)", restarted ? L" after a restart" : L"", DataDir().c_str());

    // Windows restarts the app if it ever hangs or crashes (after it has run for a minute).
    RegisterApplicationRestart(L"--restarted", RESTART_NO_PATCH | RESTART_NO_REBOOT);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMsgClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives TaskbarCreated and device broadcasts.
    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMsgClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_msgWnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    WNDCLASSEXW ww{ sizeof(ww) };
    ww.lpfnWndProc = WidgetProc;
    ww.hInstance = inst;
    ww.lpszClassName = kWidgetClass;
    ww.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ww.style = CS_DBLCLKS;
    RegisterClassExW(&ww);
    g_widget = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kWidgetClass, L"Juice", WS_POPUP, 0, 0, 10, 10,
                               nullptr, nullptr, inst, nullptr);
    if (g_widget) {
        SetLayeredWindowAttributes(g_widget, 0, 242, LWA_ALPHA);
        DWORD corner = 2;  // DWMWCP_ROUND (Windows 11)
        DwmSetWindowAttribute(g_widget, 33, &corner, sizeof(corner));
        PlaceWidget(true, true);
        if (g_set.widget) ShowWindow(g_widget, SW_SHOWNOACTIVATE);
    }

    RefreshIcons(true);
    if (g_firstRun) {
        NOTIFYICONDATAW nid;
        FillNid(nid, kPlaceholderIcon);
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcscpy(nid.szInfoTitle, L"Juice is running");
        wcscpy(nid.szInfo, L"Your devices' battery levels will appear in the taskbar and the widget. It starts with Windows.");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    SetTimer(g_msgWnd, TIMER_HOUSEKEEPING, 20 * 1000, nullptr);
    StartPoller(g_msgWnd, WM_APP_POLL);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    StopPoller();
    IconsShutdown();
    Log(L"Juice exited");
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
