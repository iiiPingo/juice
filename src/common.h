// Juice - battery levels of Anas's wireless devices in the Windows taskbar.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

enum class Kind { Headphones, Earbuds, Headset, Mouse, Gamepad, Keyboard, Other };

// One reading from one poll.
struct Reading {
    std::wstring key;    // stable identity (also decides the tray icon id)
    std::wstring name;   // shown to the user
    Kind kind = Kind::Other;
    int percent = -1;    // 0..100, -1 = unknown
    bool charging = false;
    bool asleep = false; // device known but not answering right now (show last value greyed)
    // Earbuds: per-bud detail (-1 = not reported)
    bool buds = false;
    int left = -1, right = -1, box = -1;
    bool leftChg = false, rightChg = false, boxChg = false;
    std::wstring via;    // "Bluetooth", "2.4 GHz receiver", ...
    int graceSec = 120;  // how long the last value stays on screen after the device goes quiet
    std::wstring btContainer;  // set on Windows' generic Bluetooth readings
    std::vector<std::wstring> sameAs;  // Bluetooth containers this dedicated reading replaces
    bool gamesir = false;  // a GameSir pad, from any source (see MergeGameSir)
    bool exact = false;    // the vendor's own battery value, preferred over Windows' when merging
    bool noAlert = false;  // the value may belong to someone else's device: show it, never alert on it
};

// ---- util.cpp ----
void Log(const wchar_t* fmt, ...);
std::wstring DataDir();          // folder next to the exe if writable, else %APPDATA%\Juice
std::wstring ExePath();
std::wstring Hex4(unsigned v);
std::wstring Fmt(const wchar_t* fmt, ...);
bool IsJunkName(const std::wstring& name);  // garbage a sleeping device can report, e.g. "ZZZZZZZZ"
ULONGLONG AwakeMs();             // milliseconds, not counting time the PC was asleep

// ---- icons.cpp ----
void IconsInit();
void IconsShutdown();
HICON MakeDeviceIcon(Kind kind, int pct, COLORREF fg, COLORREF fill, int size);
HICON MakeGlyphIcon(Kind kind, COLORREF fg, int size);
HICON MakeNumberIcon(const std::wstring& text, COLORREF color, int size);
enum class Sym { BudLeft, BudRight, Case, Bolt, Moon };  // small status symbols for the widget
HICON MakeSymbolIcon(Sym sym, COLORREF fg, int size);

// ---- hid.cpp ----
struct HidInfo {
    std::wstring path, product;
    USHORT vid = 0, pid = 0, page = 0, usage = 0;
    USHORT inLen = 0, outLen = 0, featLen = 0;
    int iface = -1;  // from "&mi_xx" in the path, -1 if none
    std::wstring physical;   // identifies the physical device across its interfaces
    std::wstring container;  // Windows' device container id
    bool bluetooth = false;  // this collection runs over Bluetooth
};
std::vector<HidInfo> EnumHid();
std::wstring ReceiverTag(const std::vector<HidInfo>& all, const HidInfo& c);

class Hid {
public:
    ~Hid() { Close(); }
    bool Open(const HidInfo& c, bool write = true);
    bool OpenNoAccess(const HidInfo& c);  // feature reports only
    void Close();
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    bool Failed() const { return failed_; }  // the handle died (device removed, driver stuck): reopen it
    const HidInfo& info() const { return c_; }
    bool Write(const std::vector<BYTE>& data, int timeoutMs = 500);
    // Reads one input report (leading report id 0 dropped, like hidapi). Returns bytes or -1.
    int Read(std::vector<BYTE>& out, int timeoutMs);
    void Flush();
    bool SetFeature(const std::vector<BYTE>& data);
    bool GetFeature(BYTE id, std::vector<BYTE>& out);

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    bool overlapped_ = false;
    bool failed_ = false;
    HidInfo c_;
};

// ---- pollers (devices.cpp, airpods.cpp, gamepad.cpp) ----
struct PollContext {
    std::vector<HidInfo> hid;
    std::vector<Reading> out;
    std::wstring* diag = nullptr;  // non-null while building a diagnostics report
    void D(const std::wstring& line) {
        if (diag) { *diag += line; *diag += L"\r\n"; }
    }
};

void PollBluetooth(PollContext& ctx);
void PollAirPods(PollContext& ctx);
void PollSteelSeries(PollContext& ctx);
void PollMice(PollContext& ctx);
void PollGamepads(PollContext& ctx);
int BestBattery(const std::vector<int>& levels);
void MergeGameSir(std::vector<Reading>& out, int receivers);
void PollLogitech(PollContext& ctx);   // vendors.cpp
void ShutdownPollers();

// ---- poller.cpp ----
void StartPoller(HWND notify, UINT msg);  // posts msg with a heap std::vector<Reading>* in lParam
void RequestPoll();
void RequestDiagnostics();                // posts msg with wParam=1 and a heap std::wstring* in lParam
ULONGLONG PollerHeartbeat();
bool StopPoller();                        // false if the thread is still running after the wait
