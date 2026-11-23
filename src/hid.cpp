#include "common.h"

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <cfgmgr32.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <set>

namespace {

// The USB device a HID collection belongs to, so all its interfaces count as one receiver.
std::wstring UsbParent(DEVINST inst) {
    for (int depth = 0; depth < 8; depth++) {
        wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
        if (CM_Get_Device_IDW(inst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return L"";
        std::wstring s = id;
        for (auto& ch : s) ch = towupper(ch);
        if (s.rfind(L"USB\\VID_", 0) == 0 && s.find(L"&MI_") == std::wstring::npos) return s;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS) return L"";
        inst = parent;
    }
    return L"";
}

}  // namespace

std::vector<HidInfo> EnumHid() {
    std::vector<HidInfo> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need + sizeof(wchar_t));
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, &dd)) continue;
        HidInfo c;
        c.path = det->DevicePath;
        HANDLE h = CreateFileW(c.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{ sizeof(a) };
        if (HidD_GetAttributes(h, &a)) {
            c.vid = a.VendorID;
            c.pid = a.ProductID;
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                c.page = caps.UsagePage;
                c.usage = caps.Usage;
                c.inLen = caps.InputReportByteLength;
                c.outLen = caps.OutputReportByteLength;
                c.featLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        wchar_t prod[128] = {};
        if (HidD_GetProductString(h, prod, sizeof(prod))) c.product = prod;
        CloseHandle(h);
        std::wstring lower = c.path;
        for (auto& ch : lower) ch = towlower(ch);
        size_t mi = lower.find(L"&mi_");
        if (mi != std::wstring::npos) c.iface = (int)wcstol(lower.c_str() + mi + 4, nullptr, 16);
        c.bluetooth = lower.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos ||
                      lower.find(L"00001812-0000-1000-8000-00805f9b34fb") != std::wstring::npos;
        GUID container{};
        DEVPROPTYPE t = 0;
        if (SetupDiGetDevicePropertyW(di, &dd, &DEVPKEY_Device_ContainerId, &t, (PBYTE)&container, sizeof(container), nullptr, 0)) {
            wchar_t g[64];
            StringFromGUID2(container, g, 64);
            c.container = g;
        }
        c.physical = UsbParent(dd.DevInst);
        if (c.physical.empty()) c.physical = c.container.empty() ? c.path : c.container;
        out.push_back(c);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

// "" for the first receiver of a model, "#2", "#3"... for identical ones, so each keeps its own icon.
std::wstring ReceiverTag(const std::vector<HidInfo>& all, const HidInfo& c) {
    std::set<std::wstring> ids;
    for (const HidInfo& a : all)
        if (a.vid == c.vid && a.pid == c.pid) ids.insert(a.physical);
    auto it = ids.find(c.physical);
    if (it == ids.end() || it == ids.begin()) return L"";
    return Fmt(L"#%d", (int)std::distance(ids.begin(), it) + 1);
}

bool Hid::Open(const HidInfo& c, bool write) {
    Close();
    c_ = c;
    failed_ = false;
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    h_ = CreateFileW(c.path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    overlapped_ = true;
    return ok();
}

bool Hid::OpenNoAccess(const HidInfo& c) {
    Close();
    c_ = c;
    failed_ = false;
    h_ = CreateFileW(c.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    overlapped_ = false;
    return ok();
}

void Hid::Close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

namespace {

// One overlapped request. The driver owns `ov` and `buf` until the request completes, so a request
// a driver refuses to cancel is deliberately left allocated rather than freed under it.
struct IoOp {
    OVERLAPPED ov{};
    std::vector<BYTE> buf;
    IoOp() { ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~IoOp() {
        if (ov.hEvent) CloseHandle(ov.hEvent);
    }
};

enum class IoResult { Done, TimedOut, Error, Stuck };

IoResult Completed(HANDLE h, IoOp& op, DWORD& n) {
    if (GetOverlappedResult(h, &op.ov, &n, FALSE)) return IoResult::Done;
    return GetLastError() == ERROR_OPERATION_ABORTED ? IoResult::TimedOut : IoResult::Error;
}

// Waits for the request; cancels it on timeout. Never blocks longer than timeoutMs + 1 s.
IoResult Finish(HANDLE h, IoOp& op, DWORD& n, int timeoutMs) {
    if (WaitForSingleObject(op.ov.hEvent, (DWORD)std::max(timeoutMs, 0)) == WAIT_OBJECT_0) return Completed(h, op, n);
    CancelIoEx(h, &op.ov);
    if (WaitForSingleObject(op.ov.hEvent, 1000) != WAIT_OBJECT_0) return IoResult::Stuck;
    return Completed(h, op, n);
}

}  // namespace

bool Hid::Write(const std::vector<BYTE>& data, int timeoutMs) {
    if (!ok() || !overlapped_ || !c_.outLen || data.empty()) return false;
    auto op = std::make_unique<IoOp>();
    if (!op->ov.hEvent) return false;
    op->buf.assign(c_.outLen, 0);
    memcpy(op->buf.data(), data.data(), std::min(op->buf.size(), data.size()));
    DWORD n = 0;
    IoResult res = IoResult::Error;
    if (WriteFile(h_, op->buf.data(), (DWORD)op->buf.size(), nullptr, &op->ov) || GetLastError() == ERROR_IO_PENDING)
        res = Finish(h_, *op, n, timeoutMs);
    if (res == IoResult::Stuck) {
        failed_ = true;
        op.release();
        Log(L"hid: a driver did not cancel a write");
        return false;
    }
    if (res == IoResult::Done) return true;
    // Some receivers only accept output reports through the control pipe.
    std::vector<BYTE> copy(c_.outLen, 0);
    memcpy(copy.data(), data.data(), std::min(copy.size(), data.size()));
    bool done = HidD_SetOutputReport(h_, copy.data(), (ULONG)copy.size()) != FALSE;
    if (!done && res == IoResult::Error) failed_ = true;
    return done;
}

int Hid::Read(std::vector<BYTE>& out, int timeoutMs) {
    if (!ok() || !overlapped_ || !c_.inLen) return -1;
    auto op = std::make_unique<IoOp>();
    if (!op->ov.hEvent) return -1;
    op->buf.assign(c_.inLen, 0);
    DWORD got = 0;
    IoResult res = IoResult::Error;
    if (ReadFile(h_, op->buf.data(), (DWORD)op->buf.size(), nullptr, &op->ov) || GetLastError() == ERROR_IO_PENDING)
        res = Finish(h_, *op, got, timeoutMs);
    if (res == IoResult::Stuck) {
        failed_ = true;
        op.release();
        Log(L"hid: a driver did not cancel a read");
        return -1;
    }
    if (res == IoResult::Error) failed_ = true;
    if (res != IoResult::Done || got == 0) return -1;
    size_t off = op->buf[0] == 0 ? 1 : 0;
    out.assign(op->buf.begin() + off, op->buf.begin() + got);
    return (int)out.size();
}

void Hid::Flush() {
    if (ok()) HidD_FlushQueue(h_);
}

bool Hid::SetFeature(const std::vector<BYTE>& data) {
    if (!ok() || !c_.featLen) return false;
    std::vector<BYTE> buf(c_.featLen, 0);
    memcpy(buf.data(), data.data(), std::min(buf.size(), data.size()));
    return HidD_SetFeature(h_, buf.data(), (ULONG)buf.size()) != FALSE;
}

bool Hid::GetFeature(BYTE id, std::vector<BYTE>& out) {
    if (!ok() || !c_.featLen) return false;
    out.assign(c_.featLen, 0);
    out[0] = id;
    return HidD_GetFeature(h_, out.data(), (ULONG)out.size()) != FALSE;
}
