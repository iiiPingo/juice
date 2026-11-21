// Bluetooth (Windows' own battery value), SteelSeries Arctis Nova headsets and 2.4 GHz mice.
//
// Protocol notes come from the open-source projects that worked them out first:
//   SteelSeries Nova: github.com/Sapd/HeadsetControl
//   Xenta (Attack Shark / Inphic), Compx (ATK / VXE), ATK NearLink, Rongyuan (Attack Shark / Ajazz):
//   github.com/HeyOkay/HaloBattery and github.com/krisgfx9/Wireless-Device-Battery-Monitor
#include "common.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>

namespace {

bool In(USHORT v, std::initializer_list<USHORT> l) { return std::find(l.begin(), l.end(), v) != l.end(); }

std::wstring HexBytes(const std::vector<BYTE>& b, size_t max = 24) {
    std::wstring s;
    for (size_t i = 0; i < b.size() && i < max; i++) s += Fmt(L"%02x ", b[i]);
    if (b.size() > max) s += L"...";
    return s;
}

// ---------------------------------------------------------------------------------------------
// Bluetooth: Windows stores the level it reads over HFP (classic) or the GATT Battery Service
// (LE) in a device property - the same number Settings > Bluetooth & devices shows.
// ---------------------------------------------------------------------------------------------

const DEVPROPKEY kBtBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const DEVPROPKEY kIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };
const DEVPROPKEY kClassOfDevice = { { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 10 };

std::wstring StrProp(HDEVINFO di, SP_DEVINFO_DATA* dd, const DEVPROPKEY& key) {
    DEVPROPTYPE t;
    wchar_t buf[256] = {};
    if (SetupDiGetDevicePropertyW(di, dd, &key, &t, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) &&
        (t == DEVPROP_TYPE_STRING || t == DEVPROP_TYPE_STRING_LIST))
        return buf;
    return L"";
}

Kind KindFromCod(UINT32 cod) {
    UINT32 major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
    if (major == 0x04) return Kind::Headphones;
    if (major == 0x05) {
        if ((minor & 0x0F) == 0x01 || (minor & 0x0F) == 0x02) return Kind::Gamepad;
        if ((minor >> 4) == 2) return Kind::Mouse;
        if ((minor >> 4) == 1 || (minor >> 4) == 3) return Kind::Keyboard;   // keyboard, combo
    }
    return Kind::Other;
}

Kind KindFromCategory(const std::wstring& c) {
    if (c == L"Input.Mouse") return Kind::Mouse;
    if (c == L"Input.Gaming") return Kind::Gamepad;
    if (c == L"Input.Keyboard") return Kind::Keyboard;
    if (c.rfind(L"Communication.Headset", 0) == 0 || c == L"Audio.Headphones" || c == L"Audio.Headphone") return Kind::Headphones;
    return Kind::Other;
}

Kind KindFromName(const std::wstring& n) {
    std::wstring u = n;
    for (auto& c : u) c = towupper(c);
    if (u.find(L"MOUSE") != std::wstring::npos || u.find(L"SHARK") != std::wstring::npos) return Kind::Mouse;
    if (u.find(L"GAMESIR") != std::wstring::npos || u.find(L"CONTROLLER") != std::wstring::npos) return Kind::Gamepad;
    if (u.find(L"KEYBOARD") != std::wstring::npos || u.find(L"KEYS") != std::wstring::npos) return Kind::Keyboard;
    if (u.find(L"WF-") != std::wstring::npos || u.find(L"BUDS") != std::wstring::npos || u.find(L"AIRPODS") != std::wstring::npos ||
        u.find(L"PODS") != std::wstring::npos)
        return Kind::Earbuds;
    if (u.find(L"WH-") != std::wstring::npos || u.find(L"WF-") != std::wstring::npos || u.find(L"HEAD") != std::wstring::npos ||
        u.find(L"BUDS") != std::wstring::npos)
        return Kind::Headphones;
    return Kind::Other;
}

std::wstring StripServiceSuffix(std::wstring n) {
    for (const wchar_t* suf : { L" Hands-Free AG", L" Hands-Free", L" Avrcp Transport", L" Stereo", L" Audio Gateway" }) {
        size_t l = wcslen(suf);
        if (n.size() > l && n.compare(n.size() - l, l, suf) == 0) return n.substr(0, n.size() - l);
    }
    return n;
}

}  // namespace

void PollBluetooth(PollContext& ctx) {
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return;

    struct Container {
        std::wstring name, category;
        UINT32 cod = 0;
        bool hasMain = false, connected = false, apple = false;
        int pct = -1;
        bool pctLive = false;
    };
    std::map<std::wstring, Container> cont;

    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, nullptr)) continue;
        if (wcsncmp(inst, L"BTHENUM\\", 8) && wcsncmp(inst, L"BTHLE\\", 6) && wcsncmp(inst, L"BTHLEDEVICE\\", 12)) continue;

        GUID container{};
        DEVPROPTYPE t;
        std::wstring cid = inst;
        if (SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_ContainerId, &t, (PBYTE)&container, sizeof(container), nullptr, 0)) {
            wchar_t g[64];
            StringFromGUID2(container, g, 64);
            cid = g;
        }
        Container& c = cont[cid];
        std::wstring upper = inst;
        for (auto& ch : upper) ch = towupper(ch);
        if (upper.find(L"_VID&0001004C") != std::wstring::npos) c.apple = true;

        if (!wcsncmp(inst, L"BTHENUM\\DEV_", 12) || !wcsncmp(inst, L"BTHLE\\DEV_", 10)) {
            c.hasMain = true;
            std::wstring n = StrProp(di, &dd, DEVPKEY_Device_FriendlyName);
            if (n.empty()) n = StrProp(di, &dd, DEVPKEY_Device_DeviceDesc);
            if (!n.empty()) c.name = n;
            UINT32 cod = 0;
            if (SetupDiGetDevicePropertyW(di, &dd, &kClassOfDevice, &t, (PBYTE)&cod, sizeof(cod), nullptr, 0) && t == DEVPROP_TYPE_UINT32)
                c.cod = cod;
            std::wstring cat = StrProp(di, &dd, DEVPKEY_DeviceContainer_Category);
            if (!cat.empty()) c.category = cat;
            DEVPROP_BOOLEAN conn = DEVPROP_FALSE;
            if (SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) && t == DEVPROP_TYPE_BOOLEAN)
                c.connected = conn != DEVPROP_FALSE;
        }

        BYTE pct = 0;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kBtBattery, &t, &pct, sizeof(pct), nullptr, 0) || t != DEVPROP_TYPE_BYTE) continue;
        if (pct > 100) continue;
        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, dd.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)) continue;
        // Headphones keep the level on the Hands-Free node, which can read as disconnected while
        // audio runs over A2DP - so the main node's connected flag decides (below).
        DEVPROP_BOOLEAN conn = DEVPROP_TRUE;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) || t != DEVPROP_TYPE_BOOLEAN)
            conn = DEVPROP_TRUE;
        bool live = conn != DEVPROP_FALSE;
        if (c.pct >= 0 && c.pctLive && !live) continue;  // prefer a live node's value
        c.pct = pct;
        c.pctLive = live;
        if (c.name.empty()) c.name = StripServiceSuffix(StrProp(di, &dd, DEVPKEY_Device_FriendlyName));
    }
    SetupDiDestroyDeviceInfoList(di);

    ctx.D(L"-- Bluetooth devices --");
    for (auto& [cid, c] : cont) {
        if (!c.hasMain) continue;
        ctx.D(Fmt(L"  %ls | connected=%d | battery=%d | category=%ls | cod=%06X%ls", c.name.c_str(), c.connected, c.pct, c.category.c_str(),
                  c.cod, c.apple ? L" | apple" : L""));
        if (c.apple) continue;  // AirPods: Windows has no level for them; airpods.cpp reads their broadcasts
        if (!c.connected || c.pct < 0) continue;
        Reading r;
        r.key = L"bt:" + cid;
        r.name = c.name.empty() ? L"Bluetooth device" : c.name;
        r.percent = c.pct;
        r.via = L"Bluetooth";
        r.kind = KindFromCod(c.cod);
        if (r.kind == Kind::Other) r.kind = KindFromCategory(c.category);
        if (r.kind == Kind::Other) r.kind = KindFromName(r.name);
        r.graceSec = 90;
        r.btContainer = cid;
        ctx.out.push_back(r);
    }
}

// ---------------------------------------------------------------------------------------------
// SteelSeries Arctis Nova 5 / 5P / 5X (base station, VID 1038, vendor page FFC0 on interface 3).
// Request 00 B0 -> reply [0]=B0, [1]=02 headset offline, [3]=percent, [4]=01 charging.
// ---------------------------------------------------------------------------------------------

void PollSteelSeries(PollContext& ctx) {
    std::set<USHORT> done;
    for (const HidInfo& c : ctx.hid) {
        if (c.vid != 0x1038 || done.count(c.pid)) continue;
        if (c.page != 0xFFC0 || !c.outLen || !c.inLen) continue;
        // Nova 7 / 7+ / Pro use other layouts; Anas has a Nova 5P, so the Nova 5 layout is used for
        // every other base station and checked for sanity (raw reply goes to the diagnostics).
        if (In(c.pid, { 0x220E, 0x2212, 0x2216, 0x2236, 0x2202, 0x2206, 0x220A, 0x223A, 0x227A, 0x22A4, 0x12E0, 0x225D })) continue;
        Hid d;
        if (!d.Open(c)) {
            ctx.D(Fmt(L"  steelseries %04X: open failed (%lu) - is SteelSeries GG holding it?", c.pid, GetLastError()));
            continue;
        }
        std::vector<BYTE> r;
        bool got = false;
        for (int attempt = 0; attempt < 2 && !got; attempt++) {
            d.Flush();
            if (!d.Write({ 0x00, 0xB0 })) break;
            ULONGLONG end = GetTickCount64() + 1000;
            while (!got && GetTickCount64() < end) {
                if (d.Read(r, (int)(end - GetTickCount64())) < 0) break;
                if (r.size() >= 5 && r[0] == 0xB0) got = true;
            }
        }
        ctx.D(Fmt(L"  steelseries %04X page %04X: reply %ls", c.pid, c.page, got ? HexBytes(r).c_str() : L"(none)"));
        if (!got) continue;
        done.insert(c.pid);
        if (r[1] == 0x02) continue;  // headset switched off: base station only
        if (r[3] > 100) continue;
        Reading s;
        s.key = L"steelseries:" + Hex4(c.pid);
        s.name = L"Arctis Nova 5P";
        s.kind = Kind::Headset;
        s.via = L"2.4 GHz base station";
        s.percent = r[3];
        s.charging = r[4] == 0x01;
        s.graceSec = 120;
        ctx.out.push_back(s);
    }
}

// ---------------------------------------------------------------------------------------------
// Mice on 2.4 GHz receivers.
// ---------------------------------------------------------------------------------------------

namespace {

// Xenta receivers (VID 1D57: Attack Shark X11 / X3 / R1 / X6, Inphic): the receiver pushes
// [03][model][40|41][sub][level] every ~2 s on usage page 000A. Nothing is written to it.
//   sub 01 = level, 02 = full / on the cable, 03 = charging (level frozen)
//   model 10 (X6 / R1 generation): byte 3 is a DPI stage and byte 4 the level in tens.
struct XentaListener {
    std::unique_ptr<Hid> dev;
    int level = -1;      // confirmed level
    int candidate = -1;  // a level seen once (link-up starts with a false 100 %)
    bool charging = false;
    ULONGLONG lastFrame = 0, lastLevel = 0;
    BYTE model = 0;
    std::vector<std::wstring> history;
};
std::map<std::wstring, XentaListener> g_xenta;

void PollXenta(PollContext& ctx, std::set<std::wstring>& claimed) {
    std::set<std::wstring> present;
    for (const HidInfo& c : ctx.hid) {
        if (c.vid != 0x1D57 || c.page != 0x000A || c.inLen < 5) continue;
        present.insert(c.path);
        XentaListener& l = g_xenta[c.path];
        bool fresh = !l.dev;
        if (fresh) {
            l.dev = std::make_unique<Hid>();
            if (!l.dev->Open(c, false)) {
                ctx.D(Fmt(L"  xenta %04X: open failed (%lu)", c.pid, GetLastError()));
                g_xenta.erase(c.path);
                continue;
            }
        }
        // Drain what queued up since the last poll; a new listener waits up to 6 s for frames.
        ULONGLONG until = GetTickCount64() + (fresh ? 6000 : 300);
        std::vector<BYTE> r;
        int frames = 0;
        while (GetTickCount64() < until) {
            int wait = (int)std::min<ULONGLONG>(until - GetTickCount64(), fresh ? 2500 : 50);
            if (l.dev->Read(r, wait) < 0) {
                if (!fresh || l.level >= 0) break;
                continue;
            }
            if (r.size() < 5 || r[0] != 0x03 || (r[2] != 0x40 && r[2] != 0x41)) continue;
            frames++;
            l.lastFrame = GetTickCount64();
            l.model = r[1];
            l.history.push_back(HexBytes(std::vector<BYTE>(r.begin(), r.begin() + std::min<size_t>(r.size(), 8))));
            if (l.history.size() > 8) l.history.erase(l.history.begin());
            int lvl = -1;
            if (r[1] == 0x10) {
                lvl = r[4] * 10;
            } else if (r[3] == 0x03 || r[3] == 0x02) {
                l.charging = true;  // level frozen while on the cable: keep the last one
            } else {
                lvl = r[4];
                l.charging = false;
            }
            if (lvl >= 1 && lvl <= 100) {
                if (lvl == l.candidate || (l.level >= 0 && std::abs(lvl - l.level) <= 2)) {
                    l.level = lvl;
                    l.lastLevel = l.lastFrame;
                }
                l.candidate = lvl;
            }
            if (fresh && l.level >= 0) break;
        }
        ctx.D(Fmt(L"  xenta %04X model %02X: %d new frames, level %d%ls", c.pid, l.model, frames, l.level,
                  l.charging ? L" (charging)" : L""));
        for (auto& h : l.history) ctx.D(L"      " + h);
        if (l.level < 0) continue;
        ULONGLONG age = GetTickCount64() - l.lastFrame;
        if (age > 5 * 60 * 1000) continue;  // mouse off for a while
        Reading s;
        s.key = L"mouse:xenta:" + Hex4(c.pid);
        s.name = L"Attack Shark X1";
        s.kind = Kind::Mouse;
        s.via = (c.pid == 0xFA55 || c.pid == 0x2155) ? L"USB cable" : L"2.4 GHz receiver";
        s.percent = l.level;
        s.charging = l.charging;
        s.asleep = age > 90 * 1000;
        s.graceSec = 30 * 60;
        ctx.out.push_back(s);
        claimed.insert(Hex4(c.vid) + Hex4(c.pid));
    }
    for (auto it = g_xenta.begin(); it != g_xenta.end();) it = present.count(it->first) ? std::next(it) : g_xenta.erase(it);
}

// Rongyuan receivers (Attack Shark, Ajazz and others): feature report on interface 2, page FFFF:0002.
//   SET_FEATURE [00 F7 00 ...], then GET_FEATURE 0 -> [00][ok][kbd %][mouse %][kbd offline][mouse offline]
const UINT kRongyuan[] = {
    0x0DB0197E, 0x0DB08E9A, 0x145F0376, 0x145F038D, 0x14A5C045, 0x25AA2008, 0x25AA4008, 0x25AA4018, 0x31514011, 0x31514014,
    0x31514017, 0x31514021, 0x31514027, 0x3151402D, 0x31514038, 0x31515006, 0x31515007, 0x31515008, 0x31515020, 0x31515026,
    0x3151502B, 0x31515031, 0x31515037, 0x31515038, 0x3151503D, 0x3151503F, 0x31515040, 0x31515042, 0x31515045, 0x3151504B,
    0x3151504E, 0x31515050, 0x31515052, 0x31515055, 0x3151505A, 0x342DE3E0, 0x342DE3E1, 0x342DE3E2, 0x342DE3E6, 0x342DE3E7,
    0x342DE3E8, 0x342DE3F5, 0x342DE3F6, 0x342DE3F7, 0x342DE3FB, 0x342DE3FC, 0x342DE3FD, 0x347AA302, 0x347AA305, 0x374AA205,
    0x374AA217, 0x374AA220, 0x374AA223, 0x374AA226, 0x374AA229, 0x374AA234, 0x374AA237, 0x374AA251, 0x374AA263, 0x374AA268,
    0x374AA311, 0x374AA314, 0x374AA319, 0x374AA322, 0x374AA329, 0x374AA355, 0x379A1804, 0x379A1807, 0x38A90002, 0x38EE0000,
    0x38EE0002, 0x38EE0039, 0x391D1A04, 0x391D1A08, 0x391D1A0E, 0x391D1A13, 0x391D1A25, 0x391D1A2B, 0x391D1A34, 0x391D2017,
    0x391D2A05,
};

void PollRongyuan(PollContext& ctx, std::set<std::wstring>& claimed) {
    for (const HidInfo& c : ctx.hid) {
        UINT key = ((UINT)c.vid << 16) | c.pid;
        if (std::find(std::begin(kRongyuan), std::end(kRongyuan), key) == std::end(kRongyuan)) continue;
        if (claimed.count(Hex4(c.vid) + Hex4(c.pid))) continue;
        if (c.iface != 2 || c.page != 0xFFFF || c.usage != 0x0002 || c.featLen < 7) continue;
        Hid d;
        if (!d.OpenNoAccess(c)) continue;
        std::vector<BYTE> r;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            if (attempt) Sleep(100);
            std::vector<BYTE> cmd(c.featLen, 0);
            cmd[1] = 0xF7;
            if (!d.SetFeature(cmd)) break;
            Sleep(20);
            ok = d.GetFeature(0, r) && r.size() >= 7 && (r[2] | r[3] | r[4] | r[5] | r[6]) != 0;
        }
        ctx.D(Fmt(L"  rongyuan %04X:%04X: %ls", c.vid, c.pid, ok ? HexBytes(r, 12).c_str() : L"(no reply)"));
        if (!ok) continue;
        claimed.insert(Hex4(c.vid) + Hex4(c.pid));
        if (r[5] != 0 || r[3] < 1 || r[3] > 100) continue;  // no mouse in the slot / no level
        Reading s;
        s.key = L"mouse:rongyuan:" + Hex4(c.vid) + Hex4(c.pid);
        s.name = L"Attack Shark X1";
        s.kind = Kind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.percent = r[3];
        s.graceSec = 30 * 60;
        ctx.out.push_back(s);
    }
}

// Compx family (ATK / VXE dongles VID 373B, Compx 3554 and relatives): 17-byte report 08,
//   [08][cmd][0 x14][checksum], checksum = 0x55 - sum(bytes 0..15)
//   cmd 03: online -> [6]=1 when the mouse is linked; cmd 04: battery -> [6]=%, [7]=charging
BYTE CompxSum(const BYTE* b) {
    unsigned s = 0;
    for (int i = 0; i < 16; i++) s += b[i];
    return (BYTE)(0x55 - s);
}

bool CompxQuery(Hid& d, BYTE cmd, std::vector<BYTE>& r) {
    std::vector<BYTE> req(17, 0);
    req[0] = 0x08;
    req[1] = cmd;
    req[16] = CompxSum(req.data());
    d.Flush();
    if (!d.Write(req)) return false;
    ULONGLONG end = GetTickCount64() + 500;
    while (GetTickCount64() < end) {
        if (d.Read(r, (int)(end - GetTickCount64())) < 17) return false;
        if (r[0] == 0x08 && r[1] == cmd && r[16] == CompxSum(r.data())) return true;
    }
    return false;
}

void PollCompx(PollContext& ctx, std::set<std::wstring>& claimed) {
    std::vector<const HidInfo*> colls;
    for (const HidInfo& c : ctx.hid) {
        if (!In(c.vid, { 0x3554, 0x373B, 0x3770, 0x33E4, 0x3710, 0x35BB })) continue;
        if (c.vid == 0x373B && In(c.pid, { 0x11CA, 0x11CB, 0x1203, 0x1204, 0x1216 })) continue;  // headsets / NearLink dongle
        if (!((c.outLen == 17 && c.inLen == 17) || (c.page >= 0xFF00 && c.outLen >= 17 && c.inLen >= 17))) continue;
        colls.push_back(&c);
    }
    std::stable_sort(colls.begin(), colls.end(), [](const HidInfo* a, const HidInfo* b) {
        return (a->page == 0xFF02 && a->usage == 2) > (b->page == 0xFF02 && b->usage == 2);
    });
    for (const HidInfo* c : colls) {
        std::wstring id = Hex4(c->vid) + Hex4(c->pid);
        if (claimed.count(id)) continue;
        Hid d;
        if (!d.Open(*c)) continue;
        std::vector<BYTE> r;
        bool answered = false, online = false;
        if (CompxQuery(d, 0x03, r)) {
            answered = true;
            online = r[6] != 0;
        }
        Reading s;
        bool haveLevel = false;
        if ((!answered || online) && CompxQuery(d, 0x04, r)) {
            answered = true;
            if (r[6] <= 100) {
                s.percent = r[6];
                haveLevel = true;
            }
            s.charging = r[7] != 0;
        }
        ctx.D(Fmt(L"  compx %04X:%04X page %04X: answered=%d online=%d level=%d", c->vid, c->pid, c->page, answered, online, s.percent));
        if (!answered) continue;
        claimed.insert(id);
        if (!haveLevel) continue;
        s.key = L"mouse:compx:" + id;
        s.name = c->vid == 0x373B ? L"ATK mouse" : L"Wireless mouse";
        s.kind = Kind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.graceSec = 30 * 60;
        ctx.out.push_back(s);
    }
}

// ATK NearLink "NK mouse NANO dongle" 373B:1216: 64-byte report 08 on FF05:0001,
//   [08][sum(2..63)][72][02][00][01][cmd][01]..., cmd 07 = level, 0F = charging; reply [5]=cmd, [7]=value.
int BitmouseQuery(Hid& d, BYTE cmd) {
    std::vector<BYTE> req(64, 0);
    req[0] = 0x08;
    req[2] = 0x72;
    req[3] = 0x02;
    req[5] = 0x01;
    req[6] = cmd;
    req[7] = 0x01;
    unsigned sum = 0;
    for (size_t i = 2; i < req.size(); i++) sum += req[i];
    req[1] = (BYTE)sum;
    d.Flush();
    if (!d.Write(req)) return -1;
    ULONGLONG end = GetTickCount64() + 700;
    std::vector<BYTE> r;
    while (GetTickCount64() < end) {
        if (d.Read(r, (int)(end - GetTickCount64())) < 0) return -1;
        if (r.size() < 8 || r[0] != 0x08 || r[1] != 0x72 || r[5] != cmd) continue;
        if (r[2] == 0xFF) return -1;
        return r[7];
    }
    return -1;
}

void PollBitmouse(PollContext& ctx, std::set<std::wstring>& claimed) {
    for (const HidInfo& c : ctx.hid) {
        if (c.vid != 0x373B || c.pid != 0x1216 || c.page != 0xFF05 || c.usage != 1 || c.outLen < 64) continue;
        if (claimed.count(Hex4(c.vid) + Hex4(c.pid))) continue;
        Hid d;
        if (!d.Open(c)) continue;
        int level = BitmouseQuery(d, 0x07);
        ctx.D(Fmt(L"  atk nearlink: level %d", level));
        if (level < 1 || level > 100) continue;
        Reading s;
        s.key = L"mouse:atknk";
        s.name = L"ATK mouse";
        s.kind = Kind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.percent = level;
        s.charging = BitmouseQuery(d, 0x0F) == 1;
        s.graceSec = 30 * 60;
        ctx.out.push_back(s);
        claimed.insert(Hex4(c.vid) + Hex4(c.pid));
        break;
    }
}

}  // namespace

void PollMice(PollContext& ctx) {
    std::set<std::wstring> claimed;
    ctx.D(L"-- Mouse receivers --");
    PollXenta(ctx, claimed);
    PollBitmouse(ctx, claimed);
    PollRongyuan(ctx, claimed);
    PollCompx(ctx, claimed);
}

void ShutdownMice() { g_xenta.clear(); }
