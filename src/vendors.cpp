// Logitech (HID++ 2.0) wireless mice / keyboards on Unifying, Bolt and Lightspeed receivers.
// The protocol is public (Logitech's HID++ documentation, Solaar, libratbag); LGSTrayBattery reads it the same way on Windows.
// (Razer was considered and left out: its battery command may need Razer's own driver on Windows.)
// Everything here only asks for values; nothing is written to the device's settings.
#include "common.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace {

bool InList(USHORT v, std::initializer_list<USHORT> l) { return std::find(l.begin(), l.end(), v) != l.end(); }

std::wstring Hex(const std::vector<BYTE>& b, size_t max = 20) {
    std::wstring s;
    for (size_t i = 0; i < b.size() && i < max; i++) s += Fmt(L"%02x ", b[i]);
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

Kind KindFromProduct(const std::wstring& product, Kind fallback) {
    std::wstring u = Lower(product);
    for (const wchar_t* k : { L"keyboard", L"keys", L"craft", L"k3", L"k8", L"k9" })
        if (u.find(k) != std::wstring::npos) return Kind::Keyboard;
    for (const wchar_t* m : { L"mouse", L"mx master", L"mx anywhere", L"pro x superlight", L"g pro", L"g502", L"g305", L"g703", L"g903" })
        if (u.find(m) != std::wstring::npos) return Kind::Mouse;
    return fallback;
}

// =================================================================================================
// Logitech HID++ 2.0
//   Long report: [11][device index][feature index][function << 4 | software id][16 bytes params]
//   Root feature (index 0) function 0 = getFeature(featureId) -> [feature index][type][version]
//   Error: [11][index][FF][feature index][function|sw][error code]
//   Receivers (Unifying / Bolt / Lightspeed) carry up to 6 devices at index 1..6; a device on its
//   own USB link answers at index FF.
// Battery features, newest first:
//   1004 UNIFIED_BATTERY  get_status (fn 1) -> [0] percent, [1] level flags, [2] charging state
//   1000 BATTERY_STATUS   fn 0 -> [0] percent, [1] next level, [2] status (1,2,4 = charging, 3 = full)
//   1001 BATTERY_VOLTAGE  fn 0 -> [0..1] millivolts, [2] flags (0x80 = charging)
// Name / type: 0005 DEVICE_NAME  fn 0 count, fn 1 name chunk (from offset), fn 2 type
// =================================================================================================

const BYTE kSw = 0x0A;  // our software id, so replies to Logitech G HUB / Options are never mistaken for ours

struct LogiDevice {
    BYTE battFeature = 0, battIndex = 0;  // which battery feature it has, and where
    BYTE nameIndex = 0;
    std::wstring name;
    Kind kind = Kind::Mouse;
    bool probed = false;
    ULONGLONG nextProbe = 0;  // devices that didn't answer are asked again later (they may be off)
};
std::map<std::wstring, LogiDevice> g_logi;  // key: collection path + "#" + device index

// one request / reply; false on timeout or an HID++ error
bool HidppCall(Hid& d, BYTE index, BYTE feature, BYTE function, std::initializer_list<BYTE> params, std::vector<BYTE>& reply,
               int timeoutMs = 350) {
    std::vector<BYTE> req(20, 0);
    req[0] = 0x11;
    req[1] = index;
    req[2] = feature;
    req[3] = (BYTE)((function << 4) | kSw);
    size_t i = 4;
    for (BYTE p : params) if (i < req.size()) req[i++] = p;
    if (!d.Write(req, 300)) return false;
    ULONGLONG end = GetTickCount64() + timeoutMs;
    std::vector<BYTE> r;
    while (GetTickCount64() < end) {
        if (d.Read(r, (int)(end - GetTickCount64())) < 0) return false;
        if (r.size() < 7 || (r[0] != 0x11 && r[0] != 0x10) || r[1] != index) continue;
        if (r[2] == 0xFF && r[3] == feature && r[4] == req[3]) return false;   // HID++ 2.0 error
        if (r[2] == 0x8F) return false;                                        // HID++ 1.0 error (e.g. device not connected)
        if (r[2] == feature && r[3] == req[3]) {
            reply.assign(r.begin() + 4, r.end());
            reply.resize(16, 0);
            return true;
        }
    }
    return false;
}

BYTE FeatureIndex(Hid& d, BYTE index, USHORT featureId, int timeoutMs = 350) {
    std::vector<BYTE> r;
    if (!HidppCall(d, index, 0x00, 0, { (BYTE)(featureId >> 8), (BYTE)featureId }, r, timeoutMs)) return 0;
    return r[0];
}

// Li-ion curve used by Solaar for devices that only report millivolts
int PercentFromMillivolts(int mv) {
    static const int table[][2] = { { 4186, 100 }, { 4067, 90 }, { 3989, 80 }, { 3922, 70 }, { 3859, 60 }, { 3811, 50 }, { 3778, 40 },
                                    { 3751, 30 }, { 3717, 20 }, { 3671, 10 }, { 3646, 5 }, { 3579, 2 }, { 3500, 0 } };
    if (mv >= table[0][0]) return 100;
    for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++)
        if (mv >= table[i][0]) {
            double t = double(mv - table[i][0]) / (table[i - 1][0] - table[i][0]);
            return (int)std::lround(table[i][1] + t * (table[i - 1][1] - table[i][1]));
        }
    return 0;
}

std::wstring HidppName(Hid& d, BYTE index, BYTE nameIdx) {
    std::vector<BYTE> r;
    if (!HidppCall(d, index, nameIdx, 0, {}, r)) return L"";
    int len = std::min<int>(r[0], 64);
    std::string s;
    while ((int)s.size() < len) {
        if (!HidppCall(d, index, nameIdx, 1, { (BYTE)s.size() }, r)) break;
        size_t before = s.size();
        for (BYTE b : r) {
            if ((int)s.size() >= len || b == 0) break;
            s += (char)b;
        }
        if (s.size() == before) break;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    while (!w.empty() && w.back() == L' ') w.pop_back();
    return w;
}

Kind HidppKind(Hid& d, BYTE index, BYTE nameIdx, Kind fallback) {
    std::vector<BYTE> r;
    if (!nameIdx || !HidppCall(d, index, nameIdx, 2, {}, r)) return fallback;
    switch (r[0]) {
        case 0: case 2: return Kind::Keyboard;           // keyboard, numpad
        case 3: case 4: case 5: return Kind::Mouse;      // mouse, trackpad, trackball
        case 8: return Kind::Headset;
        case 10: case 11: case 12: return Kind::Gamepad; // wheel, joystick, gamepad
        default: return fallback;
    }
}

bool IsLogiReceiver(const HidInfo& c) {
    std::wstring p = Lower(c.product);
    return p.find(L"receiver") != std::wstring::npos ||
           InList(c.pid, { 0xC52B, 0xC52F, 0xC531, 0xC532, 0xC534, 0xC537, 0xC539, 0xC53A, 0xC53D, 0xC53F, 0xC541, 0xC545, 0xC547,
                           0xC548, 0xC54D });
}

}  // namespace

void PollLogitech(PollContext& ctx) {
    ctx.D(L"-- Logitech (HID++) --");
    std::set<std::wstring> present;
    for (const HidInfo& c : ctx.hid) {
        // the vendor "long report" collection: page FF00 usage 2, 20-byte reports
        if (c.vid != 0x046D || c.page != 0xFF00 || c.usage != 0x0002 || c.outLen < 20 || c.inLen < 20) continue;
        Hid d;
        if (!d.Open(c)) {
            ctx.D(Fmt(L"  046D:%04X: open failed (%lu)", c.pid, GetLastError()));
            continue;
        }
        bool receiver = IsLogiReceiver(c);
        std::vector<BYTE> indices;
        if (receiver) indices = { 1, 2, 3, 4, 5, 6 };
        else indices = { 0xFF };
        d.Flush();
        for (BYTE idx : indices) {
            std::wstring key = c.path + L"#" + std::to_wstring(idx);
            present.insert(key);
            LogiDevice& dev = g_logi[key];
            ULONGLONG now = GetTickCount64();
            if (!dev.probed) {
                if (now < dev.nextProbe) continue;
                // which battery feature does it have? (no answer = nothing paired there, or it's off)
                for (USHORT f : { (USHORT)0x1004, (USHORT)0x1000, (USHORT)0x1001 }) {
                    BYTE fi = FeatureIndex(d, idx, f, receiver ? 250 : 400);
                    if (fi) { dev.battFeature = (BYTE)(f & 0xFF); dev.battIndex = fi; break; }
                    if (f == 0x1004) {
                        // a device that answered "not supported" is there; one that timed out isn't
                        std::vector<BYTE> r;
                        if (!HidppCall(d, idx, 0x00, 1, {}, r, receiver ? 250 : 400)) break;  // root ping (getProtocolVersion)
                    }
                }
                if (!dev.battIndex) {
                    dev.nextProbe = now + 2 * 60 * 1000;
                    ctx.D(Fmt(L"  046D:%04X #%d: no battery feature / no answer", c.pid, idx));
                    continue;
                }
                dev.nameIndex = FeatureIndex(d, idx, 0x0005);
                if (dev.nameIndex) dev.name = HidppName(d, idx, dev.nameIndex);
                dev.kind = HidppKind(d, idx, dev.nameIndex, KindFromProduct(dev.name.empty() ? c.product : dev.name, Kind::Mouse));
                if (dev.name.empty()) dev.name = receiver ? L"Logitech device" : (c.product.empty() ? L"Logitech device" : c.product);
                dev.probed = true;
            }
            std::vector<BYTE> r;
            int pct = -1;
            bool charging = false;
            bool ok = false;
            if (dev.battFeature == 0x04) {
                ok = HidppCall(d, idx, dev.battIndex, 1, {}, r);
                if (ok) {
                    pct = r[0];
                    if (pct == 0 && r[1]) pct = (r[1] & 8) ? 100 : (r[1] & 4) ? 60 : (r[1] & 2) ? 20 : 5;  // level-only devices
                    charging = r[2] == 1 || r[2] == 2;
                    if (r[2] == 3) { charging = false; pct = std::max(pct, 100); }
                }
            } else if (dev.battFeature == 0x00) {
                ok = HidppCall(d, idx, dev.battIndex, 0, {}, r);
                if (ok) {
                    pct = r[0];
                    charging = r[2] == 1 || r[2] == 2 || r[2] == 4;
                    if (r[2] == 3) pct = 100;
                }
            } else if (dev.battFeature == 0x01) {
                ok = HidppCall(d, idx, dev.battIndex, 0, {}, r);
                if (ok) {
                    int mv = (r[0] << 8) | r[1];
                    pct = mv > 0 ? PercentFromMillivolts(mv) : -1;
                    charging = (r[2] & 0x80) != 0;
                }
            }
            ctx.D(Fmt(L"  046D:%04X #%d %ls: feature 10%02X -> %ls", c.pid, idx, dev.name.c_str(), dev.battFeature,
                      ok ? Hex(r, 4).c_str() : L"(no answer: off or asleep)"));
            if (!ok) {
                // turned off or out of range: ask the features again next time it shows up
                if (receiver) { dev.probed = false; dev.nextProbe = now + 60 * 1000; }
                continue;
            }
            if (pct < 0 || pct > 100) continue;
            Reading s;
            s.key = L"logi:" + Hex4(c.pid) + L":" + std::to_wstring(idx);
            s.name = dev.name;
            s.kind = dev.kind;
            s.via = receiver ? L"Logitech receiver" : L"USB";
            s.percent = pct;
            s.charging = charging;
            s.graceSec = 30 * 60;
            ctx.out.push_back(s);
        }
    }
    for (auto it = g_logi.begin(); it != g_logi.end();) it = present.count(it->first) ? std::next(it) : g_logi.erase(it);
}

void ShutdownVendors() { g_logi.clear(); }
