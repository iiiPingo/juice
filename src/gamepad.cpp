// GameSir controllers.
//  1. Xbox mode (G7 Pro and other GameSir pads on the 2.4 GHz receiver or Bluetooth): Windows.Gaming.Input
//     gives a real battery report (remaining / full mWh).
//  2. GameSir Nova / Nova Lite / Nova 2 Lite receivers (3537:1098 / 1040, cable 100F): vendor page FF7A,
//     request 01 01 -> reply [0]=01, [18] = percent (FF = charging).
//  3. Bluetooth mode is covered by devices.cpp (Windows' own battery value).
#ifndef ____FIReference_1_boolean_INTERFACE_DEFINED__
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include "common.h"

#include <roapi.h>
#include <winstring.h>
#include <windows.system.power.h>
namespace ABI { namespace Windows { namespace Devices { namespace Power {
using ABI::Windows::System::Power::BatteryStatus;
} } } }
#include <windows.devices.power.h>
#include <windows.gaming.input.h>

#include <cwctype>
#include <set>

using namespace ABI::Windows::Gaming::Input;
using ABI::Windows::Devices::Power::IBatteryReport;

namespace {

template <class T>
void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

IRawGameControllerStatics* Statics() {
    static IRawGameControllerStatics* statics = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const wchar_t* cls = L"Windows.Gaming.Input.RawGameController";
        HSTRING name = nullptr;
        if (SUCCEEDED(WindowsCreateString(cls, (UINT32)wcslen(cls), &name))) {
            RoGetActivationFactory(name, __uuidof(IRawGameControllerStatics), (void**)&statics);
            WindowsDeleteString(name);
        }
        // The controller list fills in asynchronously after the first use; the next poll sees it.
    }
    return statics;
}

void PollWgi(PollContext& ctx) {
    IRawGameControllerStatics* statics = Statics();
    if (!statics) {
        ctx.D(L"  Windows.Gaming.Input unavailable");
        return;
    }
    __FIVectorView_1_Windows__CGaming__CInput__CRawGameController* list = nullptr;
    if (FAILED(statics->get_RawGameControllers(&list)) || !list) return;
    UINT32 count = 0;
    list->get_Size(&count);
    for (UINT32 i = 0; i < count; i++) {
        IRawGameController* raw = nullptr;
        if (FAILED(list->GetAt(i, &raw)) || !raw) continue;
        UINT16 vid = 0, pid = 0;
        raw->get_HardwareVendorId(&vid);
        raw->get_HardwareProductId(&pid);
        std::wstring name;
        IRawGameController2* raw2 = nullptr;
        if (SUCCEEDED(raw->QueryInterface(__uuidof(IRawGameController2), (void**)&raw2)) && raw2) {
            HSTRING dn = nullptr;
            if (SUCCEEDED(raw2->get_DisplayName(&dn)) && dn) {
                UINT32 len = 0;
                const wchar_t* p = WindowsGetStringRawBuffer(dn, &len);
                if (p && len) name.assign(p, len);
                WindowsDeleteString(dn);
            }
            Rel(raw2);
        }
        int pct = -1;
        INT32 remaining = 0, capacity = 0;
        bool charging = false;
        IGameControllerBatteryInfo* info = nullptr;
        IBatteryReport* report = nullptr;
        if (SUCCEEDED(raw->QueryInterface(__uuidof(IGameControllerBatteryInfo), (void**)&info)) && info &&
            SUCCEEDED(info->TryGetBatteryReport(&report)) && report) {
#ifdef _MSC_VER
            ABI::Windows::Foundation::IReference<int> *rem = nullptr, *full = nullptr;
#else
            __FIReference_1_INT32 *rem = nullptr, *full = nullptr;
#endif
            report->get_RemainingCapacityInMilliwattHours(&rem);
            report->get_FullChargeCapacityInMilliwattHours(&full);
            INT32 r = 0, f = 0;
            if (rem) rem->get_Value(&r);
            if (full) full->get_Value(&f);
            remaining = r;
            capacity = f;
            ABI::Windows::System::Power::BatteryStatus st = ABI::Windows::System::Power::BatteryStatus_NotPresent;
            report->get_Status(&st);
            if (f > 0 && st != ABI::Windows::System::Power::BatteryStatus_NotPresent) {
                pct = (int)((long long)r * 100 / f);
                if (pct > 100) pct = 100;
                if (pct < 0) pct = 0;
                charging = st == ABI::Windows::System::Power::BatteryStatus_Charging;
            }
            Rel(rem);
            Rel(full);
        }
        Rel(report);
        Rel(info);
        Rel(raw);
        ctx.D(Fmt(L"  reading [Windows.Gaming.Input] %04X:%04X \"%ls\" battery=%d%% (raw %d / %d)%ls", vid, pid, name.c_str(), pct, remaining, capacity,
                  charging ? L" charging" : L""));
        // Xbox-mode pads (GameSir in Xbox mode reports as Microsoft 045E or GameSir 3537). PlayStation
        // and Switch pads are skipped: Windows' numbers for them are not reliable.
        if (pct < 0 || (vid != 0x045E && vid != 0x3537)) continue;
        Reading s;
        s.key = L"pad:wgi:" + Hex4(vid) + Hex4(pid);
        s.name = (vid == 0x3537 || name.empty() || name.find(L"Xbox") != std::wstring::npos) ? L"GameSir controller" : name;
        s.kind = Kind::Gamepad;
        s.via = L"Xbox mode";
        std::wstring upperName = name;
        for (auto& ch : upperName) ch = towupper(ch);
        s.gamesir = vid == 0x3537 || upperName.find(L"GAMESIR") != std::wstring::npos;
        for (const HidInfo& h : ctx.hid)  // this exact pad over Bluetooth: replaces Windows' generic reading
            if (h.vid == vid && h.pid == pid && h.bluetooth) s.sameAs.push_back(h.container);
        s.percent = pct;
        s.charging = charging;
        s.graceSec = 120;
        ctx.out.push_back(s);
    }
    Rel(list);
}

void PollGameSirNova(PollContext& ctx) {
    for (const HidInfo& c : ctx.hid) {
        if (c.vid != 0x3537 || (c.pid != 0x1098 && c.pid != 0x1040 && c.pid != 0x100F)) continue;
        if (c.page != 0xFF7A || c.outLen < 3) continue;
        Hid d;
        if (!d.Open(c)) continue;
        d.Flush();
        if (!d.Write({ 0x00, 0x01, 0x01 })) continue;
        std::vector<BYTE> r;
        bool found = false;
        ULONGLONG end = GetTickCount64() + 800;
        while (!found && GetTickCount64() < end) {
            if (d.Read(r, 200) < 19) continue;
            if (r[0] == 0x01) found = true;
        }
        ctx.D(Fmt(L"  reading [GameSir vendor query] %04X: %ls", c.pid, found ? Fmt(L"level byte %u", r[18]).c_str() : L"(no reply)"));
        if (!found || (r[18] > 100 && r[18] != 0xFF)) continue;
        Reading s;
        s.key = L"pad:gamesir:" + Hex4(c.pid);
        s.name = L"GameSir controller";
        s.gamesir = true;
        s.exact = true;
        s.kind = Kind::Gamepad;
        s.via = c.pid == 0x100F ? L"USB cable" : L"2.4 GHz receiver";
        if (r[18] == 0xFF) s.charging = true;  // no level while charging: the last one is kept
        else s.percent = r[18];
        s.graceSec = 120;
        ctx.out.push_back(s);
        break;
    }
}

}  // namespace

// Windows' coarse battery steps. A number outside them is a real reading.
int BestBattery(const std::vector<int>& levels) {
    int best = -1;
    bool bestFine = false;
    for (int v : levels) {
        if (v < 0) continue;
        bool fine = v != 0 && v != 25 && v != 33 && v != 50 && v != 66 && v != 67 && v != 75 && v != 100;
        if (best < 0 || (fine && !bestFine) || (fine == bestFine && v < best)) {
            best = v;
            bestFine = fine;
        }
    }
    return best;
}

// One GameSir receiver can appear several times in Windows, each with its own battery number.
// With exactly one GameSir USB device plugged in they are the same pad, so they become one entry.
// With two or more, nothing is merged, because they may really be different controllers.
void MergeGameSir(std::vector<Reading>& out, int receivers) {
    if (receivers != 1) return;
    std::vector<size_t> pads;
    for (size_t i = 0; i < out.size(); i++)
        if (out[i].gamesir && out[i].btContainer.empty()) pads.push_back(i);
    if (pads.empty()) return;
    Reading merged = out[pads[0]];
    std::vector<int> levels;
    int exactLevel = -1;
    bool charging = false;
    std::vector<std::wstring> same;
    for (size_t i : pads) {
        const Reading& r = out[i];
        levels.push_back(r.percent);
        if (r.exact && r.percent >= 0) exactLevel = r.percent;
        charging |= r.charging;
        same.insert(same.end(), r.sameAs.begin(), r.sameAs.end());
        if (merged.name == L"GameSir controller" && r.name != merged.name) merged.name = r.name;
    }
    merged.key = L"pad:gamesir";  // stays the same whichever readings are present, so saved names and hiding stick
    merged.percent = exactLevel >= 0 ? exactLevel : BestBattery(levels);
    merged.charging = charging;
    merged.sameAs = same;
    for (auto it = pads.rbegin(); it != pads.rend(); ++it) out.erase(out.begin() + *it);
    out.push_back(merged);
}

void PollGamepads(PollContext& ctx) {
    ctx.D(L"-- Controllers --");
    PollWgi(ctx);
    PollGameSirNova(ctx);
    std::set<std::wstring> receivers;
    for (const HidInfo& h : ctx.hid)
        if (h.vid == 0x3537 && !h.bluetooth) {
            receivers.insert(h.physical);
            if (h.page == 0xFF7A) ctx.D(Fmt(L"  GameSir receiver %04X:%04X page FF7A \"%ls\"", h.vid, h.pid, h.product.c_str()));
        }
    size_t before = ctx.out.size();
    MergeGameSir(ctx.out, (int)receivers.size());
    ctx.D(Fmt(L"  GameSir USB devices: %zu, readings merged: %zu", receivers.size(), before - ctx.out.size()));
}
