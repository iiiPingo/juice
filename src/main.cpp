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
    IDM_NAMES,
    IDM_ALERT_OFF = 200,  // 200 + percent
    IDM_DEVICE = 1000,    // 1000 + 3 x device: hide / show, rename, original name
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
    bool showNames = false;  // device names in the widget
    void Load() {
        // Hand-edited or damaged values fall back to the default instead of changing behaviour.
        wchar_t raw[32] = {};
        GetPrivateProfileStringW(kAppName, L"AlertAt", L"20", raw, 32, IniPath().c_str());
        wchar_t* end = nullptr;
        long parsed = wcstol(raw, &end, 10);
        bool valid = end != raw && *end == 0 &&
                     (parsed == 0 || parsed == 10 || parsed == 15 || parsed == 20 || parsed == 25 || parsed == 30);
        alertAt = valid ? (int)parsed : 20;
        widget = GetInt(L"Widget", 1) != 0;
        onTop = GetInt(L"WidgetOnTop", 0) != 0;
        wx = GetInt(L"WidgetX", INT_MIN);
        wy = GetInt(L"WidgetY", INT_MIN);
        autostart = GetInt(L"Autostart", 1) != 0;
        showNames = GetInt(L"WidgetNames", 0) != 0;
        iconStyle = GetInt(L"IconStyle", 0) == 1 ? 1 : 0;
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
    bool alertPending = false;
};

HINSTANCE g_inst;
HWND g_msgWnd = nullptr, g_widget = nullptr;
UINT g_taskbarCreated = 0;
std::map<std::wstring, Entry> g_devs;
std::set<std::wstring> g_alerted;  // devices already warned about; survives them dropping off the list
bool g_exiting = false;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;
bool g_firstRun = false;
ULONGLONG g_startTick = 0;  // AwakeMs()
ULONGLONG g_resumeAt = 0;   // AwakeMs() of the last wake from sleep

// ---- helpers ------------------------------------------------------------------------------------

// Saved per device in settings.ini: [Names] key=custom name, [Hidden] key=name shown in the menu.
// INI files here are ANSI, so anything else (Arabic names, for example) is stored as \uXXXX.
std::map<std::wstring, std::wstring> g_names, g_hidden;

std::wstring EscapeIni(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c >= 0x20 && c < 0x7F && c != L'\\' && c != L'"' && c != L'\'' && c != L';') o += c;
        else o += Fmt(L"\\u%04X", (unsigned)c);
    }
    return o;
}

std::wstring UnescapeIni(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 5 < s.size() && s[i + 1] == L'u') {
            wchar_t hex[5] = { s[i + 2], s[i + 3], s[i + 4], s[i + 5], 0 };
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(hex, &end, 16);
            if (end == hex + 4) {
                o += (wchar_t)v;
                i += 5;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

std::map<std::wstring, std::wstring> LoadSection(const wchar_t* section) {
    std::map<std::wstring, std::wstring> out;
    std::vector<wchar_t> buf(32767);
    DWORD n = GetPrivateProfileSectionW(section, buf.data(), (DWORD)buf.size(), IniPath().c_str());
    for (DWORD i = 0; i < n && buf[i];) {
        std::wstring line = buf.data() + i;
        i += (DWORD)line.size() + 1;
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos && eq > 0) out[line.substr(0, eq)] = UnescapeIni(line.substr(eq + 1));
    }
    return out;
}

void SaveEntry(const wchar_t* section, const std::wstring& key, const std::wstring* value) {
    WritePrivateProfileStringW(section, key.c_str(), value ? EscapeIni(*value).c_str() : nullptr, IniPath().c_str());
}

std::wstring DisplayName(const Entry& e) {
    auto it = g_names.find(e.r.key);
    return it != g_names.end() ? it->second : e.r.name;
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

const wchar_t* GenericName(Kind k) {
    switch (k) {
        case Kind::Mouse: return L"Wireless mouse";
        case Kind::Keyboard: return L"Wireless keyboard";
        case Kind::Gamepad: return L"Controller";
        case Kind::Earbuds: return L"Earbuds";
        case Kind::Headphones: return L"Headphones";
        case Kind::Headset: return L"Headset";
        default: return L"Wireless device";
    }
}

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
    for (auto& [k, e] : g_devs)
        if (!IsHidden(k)) v.push_back(&e);
    std::sort(v.begin(), v.end(), [](Entry* a, Entry* b) {
        if (KindOrder(a->r.kind) != KindOrder(b->r.kind)) return KindOrder(a->r.kind) < KindOrder(b->r.kind);
        return DisplayName(*a) < DisplayName(*b);
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
    // The current scale, so icons stay sharp after a display scale change without a restart.
    UINT dpi = g_msgWnd ? GetDpiForWindow(g_msgWnd) : 0;
    int s = dpi ? GetSystemMetricsForDpi(SM_CXSMICON, dpi) : GetSystemMetrics(SM_CXSMICON);
    return s > 0 ? s : 16;
}

// ---- tray icons -----------------------------------------------------------------------------------

std::wstring TipFor(const Entry& e) {
    std::wstring t = DisplayName(e) + L"\n";
    if (e.r.buds) {
        t += L"L " + PctText(e.r.left) + (e.r.leftChg ? L"\x26A1" : L"") + L"   R " + PctText(e.r.right) + (e.r.rightChg ? L"\x26A1" : L"") +
             L"   Case " + PctText(e.r.box) + (e.r.boxChg ? L"\x26A1" : L"");
    } else {
        t += PctText(e.r.percent);
        if (e.r.charging) t += L" \x26A1 charging";
    }
    if (e.stale) {
        ULONGLONG mins = (AwakeMs() - e.lastSeen) / 60000;
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
    for (auto& [k, e] : g_devs)
        if (e.iconAdded && IsHidden(k)) {
            DeleteIcon(e.iconId);
            e.iconAdded = false;
        }
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
        g_placeholder = AddIcon(kPlaceholderIcon, g_placeholderIcon, g_devs.empty() ? L"Juice\nLooking for your devices..." : L"Juice\nAll devices are hidden");
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
    // Locked / screen off and quiet hours count too: hold the alert until it can actually be seen.
    return s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE || s == QUNS_NOT_PRESENT ||
           s == QUNS_QUIET_TIME;
}

bool ShowAlert(Entry& e) {
    NOTIFYICONDATAW nid;
    FillNid(nid, e.iconId);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING | NIIF_LARGE_ICON;
    std::wstring title = DisplayName(e) + L" is low";
    std::wstring body = e.r.buds ? (L"L " + PctText(e.r.left) + L"  R " + PctText(e.r.right) + L"  Case " + PctText(e.r.box))
                                 : (PctText(e.r.percent) + L" left. Time to charge.");
    wcsncpy(nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(nid.szInfo, body.c_str(), 255);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) return false;  // e.g. Explorer restarting: retried shortly
    Log(L"alert: %ls %d%%", e.r.name.c_str(), e.r.percent);
    return true;
}

void CheckAlerts() {
    if (g_set.alertAt <= 0) return;
    for (auto& [k, e] : g_devs) {
        int pct = e.r.percent;
        if (e.r.charging || pct >= g_set.alertAt + 5) {
            g_alerted.erase(k);  // charged again: the next low level gets a new alert
            e.alertPending = false;
            continue;
        }
        if (IsHidden(k) || e.stale || e.r.noAlert || pct < 0 || pct > g_set.alertAt || g_alerted.count(k) || !e.iconAdded) continue;
        if (UserBusy()) {
            e.alertPending = true;  // gaming / full screen / locked: tell later
            continue;
        }
        if (ShowAlert(e)) {
            g_alerted.insert(k);
            e.alertPending = false;
        } else {
            e.alertPending = true;
        }
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
    if (!g_exiting) RelaunchSelf();  // a problem while closing must not bring Juice back
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- widget ---------------------------------------------------------------------------------------

struct WidgetFonts {
    HFONT name = nullptr, pct = nullptr, tiny = nullptr;
    int dpi = 0;
} g_wf;

int Sc(int v, int dpi) { return MulDiv(v, dpi, 96); }

void EnsureFonts(int dpi) {
    if (g_wf.dpi == dpi && g_wf.name) return;
    for (HFONT* f : { &g_wf.name, &g_wf.pct, &g_wf.tiny })
        if (*f) DeleteObject(*f);
    auto mk = [&](int px, int weight, const wchar_t* face) {
        return CreateFontW(-Sc(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_wf.name = mk(13, FW_NORMAL, L"Segoe UI Variable Text");
    g_wf.pct = mk(15, FW_SEMIBOLD, L"Segoe UI Variable Text");
    g_wf.tiny = mk(12, FW_NORMAL, L"Segoe UI Variable Text");
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

HICON WidgetSymbol(Sym sym, COLORREF c, int size) {
    static std::map<std::wstring, HICON> cache;
    std::wstring key = Fmt(L"%d|%06X|%d", (int)sym, (unsigned)c, size);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HICON i = MakeSymbolIcon(sym, c, size);
    cache[key] = i;
    return i;
}

int RowHeight(const Entry& e, int dpi) { return Sc(e.r.buds ? 62 : 42, dpi); }

SIZE WidgetSize(int dpi) {
    int h = Sc(6, dpi) * 2;
    auto list = Ordered();
    if (list.empty()) h += Sc(42, dpi);
    for (Entry* e : list) h += RowHeight(*e, dpi);
    return { Sc(g_set.showNames ? 272 : 224, dpi), h };
}

void FillPill(HDC dc, const RECT& r, COLORREF c) {
    if (r.right <= r.left) return;
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ oldB = SelectObject(dc, b), oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    int h = r.bottom - r.top;
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, h, h);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(b);
}

// Each device is its symbol, a level bar and the percentage. The name stays in the tray tooltip.
void PaintWidget(HWND hwnd, HDC target) {
    int dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    EnsureFonts(dpi);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);
    HBRUSH bg = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    int pad = Sc(14, dpi);
    int y = Sc(6, dpi);
    const COLORREF chargeCol = ToneColor(Tone::Charging, false);
    auto list = Ordered();
    if (list.empty()) {
        SelectObject(dc, g_wf.name);
        SetTextColor(dc, RGB(0xA0, 0xA0, 0xA0));
        RECT t{ pad, y, rc.right - pad, y + Sc(42, dpi) };
        DrawTextW(dc, g_devs.empty() ? L"Looking for your devices..." : L"All devices are hidden", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    for (Entry* e : list) {
        const int topH = Sc(42, dpi);
        Tone tone = ToneFor(*e);
        COLORREF accent = ToneColor(tone, false);
        int gs = Sc(26, dpi);
        HICON gi = WidgetGlyph(e->r.kind, e->stale ? RGB(0x70, 0x70, 0x70) : RGB(0xF2, 0xF2, 0xF2), gs);
        if (gi) DrawIconEx(dc, pad, y + (topH - gs) / 2, gi, gs, gs, 0, nullptr, DI_NORMAL);
        int x0 = pad + gs + Sc(12, dpi);

        // Percent in a fixed column, so the bars of all rows line up.
        int pctW = Sc(44, dpi);
        RECT p{ rc.right - pad - pctW, y, rc.right - pad, y + topH };
        std::wstring pct = PctText(e->r.percent);
        SelectObject(dc, g_wf.pct);
        SetTextColor(dc, tone == Tone::Normal ? RGB(0xFF, 0xFF, 0xFF) : accent);
        DrawTextW(dc, pct.c_str(), -1, &p, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // Status: a bolt while charging, a moon while asleep.
        int barRight = p.left - Sc(8, dpi);
        int ss = Sc(14, dpi);
        if (e->stale || e->r.charging) {
            HICON si = e->stale ? WidgetSymbol(Sym::Moon, RGB(0x8C, 0x8C, 0x8C), ss) : WidgetSymbol(Sym::Bolt, chargeCol, ss);
            if (si) DrawIconEx(dc, barRight - ss, y + (topH - ss) / 2, si, ss, ss, 0, nullptr, DI_NORMAL);
            barRight -= ss + Sc(6, dpi);
        }

        // Level bar
        int barH = Sc(5, dpi), barY = g_set.showNames ? y + Sc(28, dpi) : y + (topH - barH) / 2;
        if (g_set.showNames) {
            std::wstring label = DisplayName(*e);
            SelectObject(dc, g_wf.name);
            SetTextColor(dc, e->stale ? RGB(0x90, 0x90, 0x90) : RGB(0xF0, 0xF0, 0xF0));
            RECT n{ x0, y + Sc(5, dpi), barRight, y + Sc(25, dpi) };
            DrawTextW(dc, label.c_str(), -1, &n, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        RECT track{ x0, barY, barRight, barY + barH };
        FillPill(dc, track, RGB(0x3A, 0x3A, 0x3A));
        if (e->r.percent > 0) {
            RECT fill = track;
            fill.right = track.left + std::max<int>(barH, (track.right - track.left) * std::min(e->r.percent, 100) / 100);
            FillPill(dc, fill, e->stale ? RGB(0x6A, 0x6A, 0x6A) : tone == Tone::Normal ? RGB(0xD8, 0xD8, 0xD8) : accent);
        }

        // Earbuds: left bud, right bud, case
        if (e->r.buds) {
            struct Part { Sym sym; int v; bool chg; };
            const Part parts[] = { { Sym::BudLeft, e->r.left, e->r.leftChg },
                                   { Sym::BudRight, e->r.right, e->r.rightChg },
                                   { Sym::Case, e->r.box, e->r.boxChg } };
            int colW = (rc.right - pad * 2) / 3;  // the full width, so "100%" with a bolt always fits
            int rowTop = y + topH - Sc(6, dpi), rowH = Sc(20, dpi);
            int is = Sc(15, dpi), bs = Sc(11, dpi);
            COLORREF partCol = e->stale ? RGB(0x70, 0x70, 0x70) : RGB(0xC8, 0xC8, 0xC8);
            SelectObject(dc, g_wf.tiny);
            for (int i = 0; i < 3; i++) {
                int cx = pad + colW * i;
                HICON pi = WidgetSymbol(parts[i].sym, partCol, is);
                if (pi) DrawIconEx(dc, cx, rowTop + (rowH - is) / 2, pi, is, is, 0, nullptr, DI_NORMAL);
                std::wstring txt = PctText(parts[i].v);
                SetTextColor(dc, partCol);
                RECT t{ cx + is + Sc(4, dpi), rowTop, cx + colW, rowTop + rowH };
                DrawTextW(dc, txt.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                if (parts[i].chg) {
                    SIZE sz{};
                    GetTextExtentPoint32W(dc, txt.c_str(), (int)txt.size(), &sz);
                    HICON bi = WidgetSymbol(Sym::Bolt, chargeCol, bs);
                    if (bi) DrawIconEx(dc, t.left + sz.cx + Sc(1, dpi), rowTop + (rowH - bs) / 2, bi, bs, bs, 0, nullptr, DI_NORMAL);
                }
            }
        }
        y += RowHeight(*e, dpi);
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
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

// ---- rename box -----------------------------------------------------------------------------------

struct RenameBox {
    HWND edit = nullptr;
    bool done = false, accepted = false;
    std::wstring text;
    HBRUSH bg = nullptr, field = nullptr;
};

LRESULT CALLBACK RenameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* box = (RenameBox*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_COMMAND:
            if (box && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
                box->accepted = LOWORD(wp) == IDOK;
                if (box->accepted) {
                    wchar_t buf[64] = {};
                    GetWindowTextW(box->edit, buf, 64);
                    box->text = buf;
                }
                box->done = true;
            }
            return 0;
        case WM_CLOSE:
            if (box) box->done = true;
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)wp, RGB(0xF2, 0xF2, 0xF2));
            SetBkColor((HDC)wp, RGB(0x20, 0x20, 0x20));
            return box ? (LRESULT)box->bg : 0;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wp, RGB(0xF2, 0xF2, 0xF2));
            SetBkColor((HDC)wp, RGB(0x2D, 0x2D, 0x2D));
            return box ? (LRESULT)box->field : 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Returns true when the user pressed OK. An empty result means "use the original name".
bool AskName(const std::wstring& current, std::wstring& result) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = RenameProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"JuiceRename";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
        RegisterClassExW(&wc);
        registered = true;
    }
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    using DpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = (DpiFn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
        FreeLibrary(shcore);
    }
    int dpi = (int)dpiX;
    RECT rc{ 0, 0, Sc(320, dpi), Sc(132, dpi) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_TOPMOST, dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    HWND wnd = CreateWindowExW(WS_EX_TOPMOST, L"JuiceRename", L"Rename device", style, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!wnd) return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    RenameBox box;
    box.bg = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
    box.field = CreateSolidBrush(RGB(0x2D, 0x2D, 0x2D));
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)&box);
    HFONT font = CreateFontW(-Sc(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD st, DWORD ex, int cx, int cy, int cw, int ch, UINT_PTR id) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, Sc(cx, dpi), Sc(cy, dpi), Sc(cw, dpi), Sc(ch, dpi), wnd, (HMENU)id, g_inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    child(L"STATIC", L"Name (leave empty for the original)", SS_NOPREFIX, 0, 14, 12, 292, 22, 0);
    box.edit = child(L"EDIT", current.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 0, 14, 38, 292, 28, 100);
    SendMessageW(box.edit, EM_SETLIMITTEXT, 32, 0);
    child(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 126, 86, 86, 30, IDOK);
    child(L"BUTTON", L"Cancel", WS_TABSTOP, 0, 220, 86, 86, 30, IDCANCEL);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(box.edit);
    SendMessageW(box.edit, EM_SETSEL, 0, -1);
    MSG m;
    while (!box.done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)m.wParam);  // Juice is closing: let the main loop see it
            box.done = true;
            box.accepted = false;
            break;
        }
        if (!IsDialogMessageW(wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
    DestroyWindow(wnd);
    DeleteObject(font);
    DeleteObject(box.bg);
    DeleteObject(box.field);
    if (box.accepted) result = box.text;
    return box.accepted;
}

// Every device Juice knows, including hidden ones that are switched off right now.
struct KnownDevice {
    std::wstring key, label;
    bool hidden, present;
};

std::vector<KnownDevice> KnownDevices() {
    std::vector<KnownDevice> v;
    for (auto& [k, e] : g_devs) v.push_back({ k, DisplayName(e), IsHidden(k), true });
    for (auto& [k, label] : g_hidden)
        if (!g_devs.count(k)) {
            auto n = g_names.find(k);
            v.push_back({ k, n != g_names.end() ? n->second : label, true, false });
        }
    std::sort(v.begin(), v.end(), [](const KnownDevice& a, const KnownDevice& b) { return a.label < b.label; });
    return v;
}

std::wstring MenuText(std::wstring s) {
    for (size_t p = 0; (p = s.find(L'&', p)) != std::wstring::npos; p += 2) s.insert(p, 1, L'&');
    return s;
}

void SaveStatus();  // below, beside ApplyReadings

void DevicesChanged() {
    RefreshIcons(false);
    if (g_widget) {
        if (IsWindowVisible(g_widget)) PlaceWidget(false);
        InvalidateRect(g_widget, nullptr, FALSE);
    }
    SaveStatus();
}

void HandleDeviceCommand(const KnownDevice& d, int action) {
    if (action == 0) {  // hide / show
        if (d.hidden) {
            g_hidden.erase(d.key);
            SaveEntry(L"Hidden", d.key, nullptr);
        } else {
            g_hidden[d.key] = d.label;
            SaveEntry(L"Hidden", d.key, &d.label);
        }
    } else if (action == 1) {  // rename
        std::wstring name;
        if (!AskName(d.label, name)) return;
        while (!name.empty() && name.back() == L' ') name.pop_back();
        while (!name.empty() && name.front() == L' ') name.erase(0, 1);
        auto dev = g_devs.find(d.key);
        if (name.empty() || (dev != g_devs.end() && name == dev->second.r.name)) {
            g_names.erase(d.key);
            SaveEntry(L"Names", d.key, nullptr);
        } else {
            g_names[d.key] = name;
            SaveEntry(L"Names", d.key, &name);
        }
        if (IsHidden(d.key)) {  // keep the menu label right while it is hidden
            std::wstring label = name.empty() && dev != g_devs.end() ? dev->second.r.name : name;
            if (!label.empty()) {
                g_hidden[d.key] = label;
                SaveEntry(L"Hidden", d.key, &label);
            }
        }
    } else {  // original name
        g_names.erase(d.key);
        SaveEntry(L"Names", d.key, nullptr);
    }
    DevicesChanged();
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
    AppendMenuW(m, MF_STRING | (g_set.showNames ? MF_CHECKED : 0), IDM_NAMES, L"Show device names");
    auto known = KnownDevices();
    HMENU devices = CreatePopupMenu();
    for (size_t i = 0; i < known.size(); i++) {
        const KnownDevice& d = known[i];
        HMENU sub = CreatePopupMenu();
        UINT base = IDM_DEVICE + (UINT)i * 3;
        AppendMenuW(sub, MF_STRING | (d.hidden ? MF_CHECKED : 0), base, L"Hidden");
        AppendMenuW(sub, MF_STRING, base + 1, L"Rename...");
        AppendMenuW(sub, MF_STRING | (g_names.count(d.key) ? 0 : MF_GRAYED), base + 2, L"Use original name");
        std::wstring label = MenuText(d.label) + (d.hidden ? L"  (hidden)" : L"");
        AppendMenuW(devices, MF_POPUP, (UINT_PTR)sub, label.c_str());
    }
    if (known.empty()) AppendMenuW(devices, MF_STRING | MF_GRAYED, 0, L"No devices yet");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)devices, L"Devices");
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
    } else if (cmd == IDM_NAMES) {
        g_set.showNames = !g_set.showNames;
        SetInt(L"WidgetNames", g_set.showNames);
        DevicesChanged();
    } else if (cmd >= IDM_DEVICE && (cmd - IDM_DEVICE) / 3 < known.size()) {
        HandleDeviceCommand(known[(cmd - IDM_DEVICE) / 3], (int)((cmd - IDM_DEVICE) % 3));
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
        g_alerted.clear();
        RefreshIcons(true);
        CheckAlerts();
        if (g_widget) InvalidateRect(g_widget, nullptr, FALSE);
    }
}

// ---- readings -------------------------------------------------------------------------------------

// status.txt for Minibar: the devices shown in the tray (hidden ones left out), with their shown names.
void SaveStatus() {
    std::vector<StatusDev> v;
    for (Entry* e : Ordered()) v.push_back({ DisplayName(*e), e->r.kind, e->r.percent, e->r.charging });
    if (!WriteStatus(v)) {
        static bool logged = false;
        if (!logged) Log(L"couldn't write %ls", StatusPath().c_str());
        logged = true;
    }
}

void ApplyReadings(std::vector<Reading>& readings) {
    ULONGLONG now = AwakeMs();  // time the PC slept doesn't count as a device being gone
    std::set<std::wstring> seen;
    for (Reading& r : readings) {
        seen.insert(r.key);
        Entry& e = g_devs[r.key];
        int oldPct = e.r.percent;
        bool isNew = e.lastSeen == 0;
        std::wstring oldName = e.r.name;
        e.r = r;
        if (e.r.name.empty()) e.r.name = !oldName.empty() ? oldName : GenericName(e.r.kind);  // junk name from a sleeping device
        if (e.r.percent < 0 && oldPct >= 0) e.r.percent = oldPct;  // e.g. charging without a level: keep the last one
        e.stale = r.asleep;
        if (!r.asleep) e.lastSeen = now;
        else if (isNew) e.lastSeen = now;
        if (isNew) Log(L"found %ls (%ls): %d%%", e.r.name.c_str(), r.via.c_str(), r.percent);
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
    SaveStatus();
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
                ULONGLONG now = AwakeMs();
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
        case WM_DPICHANGED:
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
            ClearStatus();   // Minibar hides its Devices block once Juice is gone
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
        // Only someone opening Juice by hand means "show the widget"; a startup launch must not.
        bool background = restarted || (cmdLine && wcsstr(cmdLine, L"--autostart"));
        HWND other = FindWindowW(kMsgClass, nullptr);
        if (other && !background) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IconsInit();
    g_firstRun = GetInt(L"FirstRunDone", 0) == 0;
    g_set.Load();
    g_names = LoadSection(L"Names");
    g_hidden = LoadSection(L"Hidden");
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
    g_exiting = true;
    if (!StopPoller()) {
        // A device call is still blocked. Freeing shared state under it could crash, so leave now.
        Log(L"Juice exited (poller still busy)");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    IconsShutdown();
    Log(L"Juice exited");
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
