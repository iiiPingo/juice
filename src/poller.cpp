// Background thread: polls every device every 45 s (sooner on request) and hands the readings to
// the UI thread. Every poller is isolated, so one misbehaving device cannot stop the others.
#include "common.h"

#include <roapi.h>

#include <algorithm>
#include <atomic>
#include <exception>

void ShutdownMice();
void ShutdownVendors();
void ShutdownAirPods();

namespace {

HWND g_notify = nullptr;
UINT g_msg = 0;
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{ false };
std::atomic<bool> g_diag{ false };
std::atomic<ULONGLONG> g_heartbeat{ 0 };

const DWORD kIntervalMs = 45 * 1000;

const wchar_t* KindName(Kind k) {
    switch (k) {
        case Kind::Headphones: return L"headphones";
        case Kind::Earbuds: return L"earbuds";
        case Kind::Headset: return L"headset";
        case Kind::Mouse: return L"mouse";
        case Kind::Gamepad: return L"controller";
        case Kind::Keyboard: return L"keyboard";
        default: return L"other";
    }
}

void Run(const wchar_t* name, void (*fn)(PollContext&), PollContext& ctx) {
    ULONGLONG t0 = GetTickCount64();
    try {
        fn(ctx);
    } catch (const std::exception& e) {
        Log(L"%ls poller threw: %hs", name, e.what());
    } catch (...) {
        Log(L"%ls poller threw an unknown exception", name);
    }
    ULONGLONG dt = GetTickCount64() - t0;
    if (dt > 8000) Log(L"%ls poller took %llu ms", name, dt);
    g_heartbeat = AwakeMs();
}

DWORD WINAPI ThreadMain(void*) {
    RoInitialize(RO_INIT_MULTITHREADED);
    Log(L"poller started");
    while (!g_stop) {
        g_heartbeat = AwakeMs();
        bool diag = g_diag.exchange(false);
        std::wstring report;
        PollContext ctx;
        if (diag) ctx.diag = &report;
        try {
            ctx.hid = EnumHid();
        } catch (...) {
            Log(L"HID enumeration threw");
        }
        Run(L"bluetooth", PollBluetooth, ctx);
        Run(L"airpods", PollAirPods, ctx);
        Run(L"steelseries", PollSteelSeries, ctx);
        Run(L"mice", PollMice, ctx);
        Run(L"logitech", PollLogitech, ctx);
        Run(L"gamepads", PollGamepads, ctx);

        // The same device seen twice (e.g. a controller in Xbox mode over Bluetooth): keep the
        // dedicated reading, drop Windows' generic Bluetooth one.
        auto has = [&](Kind k, bool dedicated) {
            return std::any_of(ctx.out.begin(), ctx.out.end(),
                               [&](const Reading& r) { return r.kind == k && (r.btContainer.empty() == dedicated); });
        };
        for (Kind k : { Kind::Gamepad, Kind::Mouse }) {
            if (!has(k, true)) continue;
            ctx.out.erase(std::remove_if(ctx.out.begin(), ctx.out.end(),
                                         [&](const Reading& r) { return r.kind == k && !r.btContainer.empty(); }),
                          ctx.out.end());
        }

        if (diag) {
            std::wstring head = L"Juice diagnostics\r\n=================\r\n\r\n-- Readings --\r\n";
            for (auto& r : ctx.out) {
                head += Fmt(L"  %ls [%ls] %d%%%ls via %ls", r.name.c_str(), KindName(r.kind), r.percent, r.charging ? L" charging" : L"",
                            r.via.c_str());
                if (r.buds) head += Fmt(L"  (L %d, R %d, case %d)", r.left, r.right, r.box);
                head += L"\r\n";
            }
            if (ctx.out.empty()) head += L"  (none)\r\n";
            head += L"\r\n";
            std::wstring hidList = L"-- HID collections (vendor id : product id, usage page : usage, in/out/feature bytes) --\r\n";
            for (auto& c : ctx.hid)
                hidList += Fmt(L"  %04X:%04X  page %04X:%04X  in %u out %u feat %u  iface %d  \"%ls\"\r\n", c.vid, c.pid, c.page, c.usage,
                               c.inLen, c.outLen, c.featLen, c.iface, c.product.c_str());
            auto* text = new std::wstring(head + report + L"\r\n" + hidList);
            if (!PostMessageW(g_notify, g_msg, 1, (LPARAM)text)) delete text;
        }
        auto* res = new std::vector<Reading>(std::move(ctx.out));
        if (!PostMessageW(g_notify, g_msg, 0, (LPARAM)res)) delete res;

        WaitForSingleObject(g_wake, kIntervalMs);
    }
    try {
        ShutdownAirPods();
        ShutdownMice();
        ShutdownVendors();
    } catch (...) {
    }
    RoUninitialize();
    return 0;
}

}  // namespace

void StartPoller(HWND notify, UINT msg) {
    g_notify = notify;
    g_msg = msg;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_heartbeat = AwakeMs();
    g_thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
}

void RequestPoll() {
    if (g_wake) SetEvent(g_wake);
}

void RequestDiagnostics() {
    g_diag = true;
    RequestPoll();
}

ULONGLONG PollerHeartbeat() { return g_heartbeat; }

void StopPoller() {
    g_stop = true;
    RequestPoll();
    if (g_thread && WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0) Log(L"poller did not stop in time");
}

void ShutdownPollers() {}
