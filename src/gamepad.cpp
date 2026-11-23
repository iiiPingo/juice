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
        ctx.D(Fmt(L"  controller %04X:%04X \"%ls\" battery=%d%ls", vid, pid, name.c_str(), pct, charging ? L" charging" : L""));
        // Xbox-mode pads (GameSir in Xbox mode reports as Microsoft 045E or GameSir 3537). PlayStation
        // and Switch pads are skipped: Windows' numbers for them are not reliable.
        if (pct < 0 || (vid != 0x045E && vid != 0x3537)) continue;
        Reading s;
        s.key = L"pad:wgi:" + Hex4(vid) + Hex4(pid);
        s.name = (vid == 0x3537 || name.empty() || name.find(L"Xbox") != std::wstring::npos) ? L"GameSir controller" : name;
        s.kind = Kind::Gamepad;
        s.via = L"Xbox mode";
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
        ctx.D(Fmt(L"  gamesir nova %04X: %ls", c.pid, found ? Fmt(L"level byte %u", r[18]).c_str() : L"(no reply)"));
        if (!found || (r[18] > 100 && r[18] != 0xFF)) continue;
        Reading s;
        s.key = L"pad:gamesir:" + Hex4(c.pid);
        s.name = L"GameSir controller";
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

void PollGamepads(PollContext& ctx) {
    ctx.D(L"-- Controllers --");
    PollWgi(ctx);
    PollGameSirNova(ctx);
}
