// AirPods (Pro 3 and friends) over Bluetooth LE.
//
// Windows never learns the AirPods' battery (Apple sends it with its own HFP commands), but the
// AirPods broadcast it in clear, a few times a second, in the BLE "proximity pairing" message
// iPhones use for their battery popup: manufacturer data of company 0x004C (Apple), TLV type 07:
//   [0] 07 [1] 19 [2] 01  [3..4] model (LE; 0x2027 = AirPods Pro 3)
//   [5] status: bit 5 = the left bud is the one broadcasting
//   [6] low nibble = broadcasting bud, high nibble = the other bud (0..10 = x10 %, 15 = unknown)
//   [7] high nibble: bit0 broadcasting bud charging, bit1 other bud charging, bit2 case charging;
//       low nibble = case level
// Layout as documented by OpenPods, AirPodsDesktop, librepods and Wireless-Device-Battery-Monitor.
// Nearby AirPods of other people broadcast the same thing, so only the model of a paired device is
// accepted and the strongest recent signal wins.
#include "common.h"

#include <eventtoken.h>
#include <inspectable.h>
#include <roapi.h>
#include <setupapi.h>
#include <winstring.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <mutex>

// Minimal WinRT ABI (vtable order from the Windows SDK). Kept out of an anonymous namespace so GCC
// does not devirtualize calls through interfaces nothing in this program implements.
namespace juice_ble {

const GUID IID_Watcher = { 0xa6ac336f, 0xf3d3, 0x4297, { 0x8d, 0x6c, 0xc8, 0x1e, 0xa6, 0x62, 0x3f, 0x40 } };
const GUID IID_ReceivedArgs = { 0x27987ddf, 0xe596, 0x41be, { 0x8d, 0x43, 0x9e, 0x67, 0x31, 0xd4, 0xa9, 0x13 } };
const GUID IID_ReceivedHandler = { 0x90eb4eca, 0xd465, 0x5ea0, { 0xa6, 0x1c, 0x03, 0x3c, 0x8c, 0x5e, 0xce, 0xf2 } };
const GUID IID_BufferByteAccess = { 0x905a0fef, 0xbc53, 0x11df, { 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda } };
const GUID IID_IUnknownJ = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID IID_IAgileObjectJ = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

struct IBufferAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32 v) = 0;
};
struct IBufferByteAccessAbi : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** v) = 0;
};
struct IManufacturerData : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_CompanyId(UINT16* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_CompanyId(UINT16 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Data(IBufferAbi** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Data(IBufferAbi* v) = 0;
};
struct IManufacturerDataView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, IManufacturerData** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* v) = 0;
};
struct IAdvertisement : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Flags(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Flags(IInspectable* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LocalName(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LocalName(HSTRING v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ServiceUuids(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ManufacturerData(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DataSections(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetManufacturerDataByCompanyId(UINT16 company, IManufacturerDataView** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSectionsByType(BYTE type, IInspectable** v) = 0;
};
struct IReceivedArgs : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_RawSignalStrengthInDBm(INT16* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BluetoothAddress(UINT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AdvertisementType(INT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Timestamp(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Advertisement(IAdvertisement** v) = 0;
};
struct IWatcher : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MinSamplingInterval(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSamplingInterval(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinOutOfRangeTimeout(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxOutOfRangeTimeout(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ScanningMode(INT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ScanningMode(INT32 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SignalStrengthFilter(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SignalStrengthFilter(IInspectable* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AdvertisementFilter(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AdvertisementFilter(IInspectable* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Received(IUnknown* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Received(EventRegistrationToken token) = 0;
};

}  // namespace juice_ble

using namespace juice_ble;

namespace {

struct Advert {
    UINT16 model = 0;
    int left = -1, right = -1, box = -1;
    bool leftChg = false, rightChg = false, boxChg = false;
    int rssi = -127;
    ULONGLONG tick = 0;
};

std::mutex g_lock;
std::map<UINT64, Advert> g_adverts;  // by rotating random BLE address (never shown or logged)
ULONGLONG g_advertsSeen = 0;

int Level(int nibble) { return nibble <= 10 ? nibble * 10 : -1; }

bool ParseTlv(const BYTE* d, UINT32 n, Advert& a) {
    if (n < 9 || d[0] != 0x07 || d[2] != 0x01) return false;
    a.model = (UINT16)(d[3] | d[4] << 8);
    if ((a.model & 0xFF00) != 0x2000) return false;
    bool leftBroadcasts = (d[5] & 0x20) != 0;
    int cur = d[6] & 0x0F, other = d[6] >> 4, chg = d[7] >> 4;
    a.left = Level(leftBroadcasts ? cur : other);
    a.right = Level(leftBroadcasts ? other : cur);
    a.leftChg = a.left >= 0 && (chg & (leftBroadcasts ? 0x01 : 0x02));
    a.rightChg = a.right >= 0 && (chg & (leftBroadcasts ? 0x02 : 0x01));
    a.box = Level(d[7] & 0x0F);
    a.boxChg = a.box >= 0 && (chg & 0x04);
    return a.left >= 0 || a.right >= 0 || a.box >= 0;
}

bool Parse(const BYTE* buf, UINT32 n, Advert& a) {
    for (UINT32 i = 0; i + 1 < n;) {
        BYTE type = buf[i], len = buf[i + 1];
        if (i + 2 + (UINT32)len > n) break;
        if (type == 0x07 && ParseTlv(buf + i, 2 + (UINT32)len, a)) return true;
        i += 2 + (UINT32)len;
    }
    return false;
}

template <class T>
void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void OnReceived(IReceivedArgs* args) {
    IAdvertisement* adv = nullptr;
    IManufacturerDataView* view = nullptr;
    if (FAILED(args->get_Advertisement(&adv)) || !adv) return;
    if (SUCCEEDED(adv->GetManufacturerDataByCompanyId(0x004C, &view)) && view) {
        UINT32 count = 0;
        view->get_Size(&count);
        for (UINT32 i = 0; i < count; i++) {
            IManufacturerData* md = nullptr;
            IBufferAbi* buf = nullptr;
            IBufferByteAccessAbi* bytes = nullptr;
            BYTE* p = nullptr;
            UINT32 len = 0;
            Advert a;
            if (SUCCEEDED(view->GetAt(i, &md)) && md && SUCCEEDED(md->get_Data(&buf)) && buf && SUCCEEDED(buf->get_Length(&len)) &&
                SUCCEEDED(buf->QueryInterface(IID_BufferByteAccess, (void**)&bytes)) && bytes && SUCCEEDED(bytes->Buffer(&p)) && p &&
                Parse(p, len, a)) {
                UINT64 addr = 0;
                INT16 rssi = -127;
                args->get_BluetoothAddress(&addr);
                args->get_RawSignalStrengthInDBm(&rssi);
                a.rssi = rssi;
                a.tick = GetTickCount64();
                std::lock_guard<std::mutex> lock(g_lock);
                g_adverts[addr] = a;
                g_advertsSeen++;
                for (auto it = g_adverts.begin(); it != g_adverts.end();)
                    it = a.tick - it->second.tick > 10 * 60 * 1000 ? g_adverts.erase(it) : std::next(it);
            }
            Rel(bytes);
            Rel(buf);
            Rel(md);
        }
    }
    Rel(view);
    Rel(adv);
}

struct Handler : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknownJ || iid == IID_ReceivedHandler || iid == IID_IAgileObjectJ) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable* argsObj) {
        IReceivedArgs* args = nullptr;
        if (argsObj && SUCCEEDED(argsObj->QueryInterface(IID_ReceivedArgs, (void**)&args)) && args) {
            OnReceived(args);
            args->Release();
        }
        return S_OK;
    }
};

Handler g_handler;
IWatcher* g_watcher = nullptr;
EventRegistrationToken g_token{};

// Starts the watcher, or restarts it after Bluetooth was switched off / the PC slept.
// Returns true when it was (re)started just now.
bool EnsureWatcher() {
    if (!g_watcher) {
        const wchar_t* cls = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
        HSTRING name = nullptr;
        IInspectable* obj = nullptr;
        if (SUCCEEDED(WindowsCreateString(cls, (UINT32)wcslen(cls), &name))) {
            RoActivateInstance(name, &obj);
            WindowsDeleteString(name);
        }
        if (!obj) return false;
        obj->QueryInterface(IID_Watcher, (void**)&g_watcher);
        obj->Release();
        if (!g_watcher) return false;
        if (FAILED(g_watcher->add_Received(&g_handler, &g_token))) {
            Rel(g_watcher);
            return false;
        }
        g_watcher->put_ScanningMode(0);  // passive: just listen
        HRESULT hr = g_watcher->Start();
        Log(L"AirPods: BLE watcher started (hr=%08lx)", (unsigned long)hr);
        return SUCCEEDED(hr);
    }
    INT32 st = 0;
    // Created (0) means the first Start failed, e.g. Bluetooth was off when Juice started.
    if (SUCCEEDED(g_watcher->get_Status(&st)) && (st == 0 /*Created*/ || st == 3 /*Stopped*/ || st == 4 /*Aborted*/)) {
        HRESULT hr = g_watcher->Start();
        Log(L"AirPods: BLE watcher restarted after status %d (hr=%08lx)", st, (unsigned long)hr);
        return SUCCEEDED(hr);
    }
    return false;
}

const DEVPROPKEY kIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };

struct Paired {
    std::wstring name, container;
    UINT16 pid = 0;
    bool connected = false;
};

std::vector<Paired> PairedAirPods() {
    std::map<std::wstring, Paired> byContainer;
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return {};
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, nullptr)) continue;
        GUID container{};
        DEVPROPTYPE t;
        if (!SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_ContainerId, &t, (PBYTE)&container, sizeof(container), nullptr, 0)) continue;
        wchar_t g[64];
        StringFromGUID2(container, g, 64);
        Paired& p = byContainer[g];
        p.container = g;
        std::wstring id = inst;
        for (auto& c : id) c = towupper(c);
        size_t v = id.find(L"_VID&0001004C");
        if (v != std::wstring::npos) {
            size_t pp = id.find(L"_PID&", v);
            if (pp != std::wstring::npos) p.pid = (UINT16)wcstoul(id.c_str() + pp + 5, nullptr, 16);
        }
        if (!wcsncmp(inst, L"BTHENUM\\DEV_", 12)) {
            wchar_t buf[256] = {};
            if (SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_FriendlyName, &t, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0))
                p.name = buf;
            DEVPROP_BOOLEAN conn = DEVPROP_FALSE;
            if (SetupDiGetDevicePropertyW(di, &dd, &kIsConnected, &t, (PBYTE)&conn, sizeof(conn), nullptr, 0) && t == DEVPROP_TYPE_BOOLEAN)
                p.connected = conn != DEVPROP_FALSE;
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    std::vector<Paired> out;
    for (auto& [cid, p] : byContainer) {
        std::wstring n = p.name;
        for (auto& c : n) c = towupper(c);
        bool appleAudio = (p.pid & 0xFF00) == 0x2000;
        if (appleAudio || (p.connected && !p.pid && n.find(L"AIRPODS") != std::wstring::npos)) out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const Paired& a, const Paired& b) { return a.connected > b.connected; });
    return out;
}

// Levels move slowly and in steps of 10, so a jump means a different pair.
bool Differs(int a, int b) { return a >= 0 && b >= 0 && std::abs(a - b) > 10; }
bool Conflicts(const Advert& a, const Advert& b) { return Differs(a.left, b.left) || Differs(a.right, b.right); }

std::map<std::wstring, Advert> g_lastGood;  // by container: the last values shown for each pair

// Best broadcast for one paired device; missing buds/case are filled from the other bud's broadcast.
// `sure` is false when another pair of the same model nearby could be the one we picked.
bool Pick(const Paired& dev, Advert& best, bool* sure = nullptr) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG maxAge = dev.connected ? 5 * 60 * 1000 : 90 * 1000;
    int minRssi = dev.connected ? -90 : -78;
    bool found = false;
    auto lastIt = g_lastGood.find(dev.container);
    auto matchesLast = [&](const Advert& a) { return lastIt != g_lastGood.end() && !Conflicts(a, lastIt->second); };
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [addr, a] : g_adverts) {
        if (dev.pid && a.model != dev.pid) continue;
        ULONGLONG age = now - a.tick;
        if (age > maxAge || a.rssi < minRssi) continue;
        bool fresh = age < 90 * 1000, bestFresh = found && now - best.tick < 90 * 1000;
        bool likeLast = matchesLast(a), bestLikeLast = found && matchesLast(best);
        if (!found || (fresh && !bestFresh) || (fresh == bestFresh && likeLast && !bestLikeLast) ||
            (fresh == bestFresh && likeLast == bestLikeLast && (fresh ? a.rssi > best.rssi : a.tick > best.tick))) {
            best = a;
            found = true;
        }
    }
    if (!found) return false;
    bool ambiguous = false;
    for (auto& [addr, a] : g_adverts)
        if (a.model == best.model && now - a.tick < 90 * 1000 && a.rssi >= best.rssi - 6 && Conflicts(a, best)) ambiguous = true;
    if (!ambiguous) {
        // Only a similar signal can be the other bud of the same pair.
        for (auto& [addr, a] : g_adverts) {
            if (a.model != best.model || std::abs(a.rssi - best.rssi) > 10) continue;
            ULONGLONG d = best.tick > a.tick ? best.tick - a.tick : a.tick - best.tick;
            if (d > 15 * 1000) continue;
            if (best.left < 0 && a.left >= 0) { best.left = a.left; best.leftChg = a.leftChg; }
            if (best.right < 0 && a.right >= 0) { best.right = a.right; best.rightChg = a.rightChg; }
            if (best.box < 0 && a.box >= 0) { best.box = a.box; best.boxChg = a.boxChg; }
        }
    }
    if (sure) *sure = dev.connected && !ambiguous && now - best.tick < 90 * 1000;
    return true;
}

// Windows adds " - Find My" to the name of some AirPods.
std::wstring CleanName(std::wstring n) {
    size_t p = n.find(L" - Find My");
    if (p != std::wstring::npos) n.resize(p);
    while (!n.empty() && n.back() == L' ') n.pop_back();
    return n;
}

}  // namespace

void PollAirPods(PollContext& ctx) {
    auto devices = PairedAirPods();
    ctx.D(L"-- AirPods --");
    for (auto& d : devices) ctx.D(Fmt(L"  paired: %ls | model %04X | connected=%d", d.name.c_str(), d.pid, d.connected));
    if (devices.empty()) return;
    if (EnsureWatcher()) {
        for (int i = 0; i < 20; i++) {  // just started: give them ~4 s to broadcast
            Advert a;
            bool any = false;
            for (auto& d : devices) any |= Pick(d, a);
            if (any) break;
            Sleep(200);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ctx.D(Fmt(L"  broadcasts received since start: %llu, recent senders: %zu", g_advertsSeen, g_adverts.size()));
        for (auto& [addr, a] : g_adverts)
            ctx.D(Fmt(L"    model %04X rssi %d age %llus  L %d R %d case %d", a.model, a.rssi, (GetTickCount64() - a.tick) / 1000, a.left,
                      a.right, a.box));
    }
    std::map<UINT16, bool> used;
    for (auto& d : devices) {
        if (d.pid && !d.connected && used[d.pid]) continue;
        Advert a;
        bool sure = false;
        if (!Pick(d, a, &sure)) continue;
        used[d.pid] = true;
        if (sure) g_lastGood[d.container] = a;
        Reading s;
        s.noAlert = !sure;
        s.name = CleanName(d.name);
        if (s.name.empty()) s.name = L"AirPods";
        s.key = L"airpods:" + Hex4(d.pid) + L":" + d.container;
        s.kind = Kind::Earbuds;
        s.via = L"Bluetooth LE";
        s.buds = true;
        s.left = a.left; s.right = a.right; s.box = a.box;
        s.leftChg = a.leftChg; s.rightChg = a.rightChg; s.boxChg = a.boxChg;
        // Headline number: the lower bud that is not charging (the one you will run out of first).
        int lo = -1;
        bool loChg = false;
        for (auto [v, c] : { std::pair<int, bool>{ a.left, a.leftChg }, std::pair<int, bool>{ a.right, a.rightChg } }) {
            if (v < 0) continue;
            if (lo < 0 || (!c && loChg) || (c == loChg && v < lo)) { lo = v; loChg = c; }
        }
        if (lo < 0) { lo = a.box; loChg = a.boxChg; }
        s.percent = lo;
        s.charging = loChg;
        s.graceSec = d.connected ? 300 : 60;
        ctx.out.push_back(s);
    }
}

void ShutdownAirPods() {
    if (g_watcher) {
        g_watcher->Stop();
        g_watcher->remove_Received(g_token);
        Rel(g_watcher);
    }
}
