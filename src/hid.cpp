#include "common.h"

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <setupapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

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
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, nullptr)) continue;
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
        out.push_back(c);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

bool Hid::Open(const HidInfo& c, bool write) {
    Close();
    c_ = c;
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    h_ = CreateFileW(c.path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    overlapped_ = true;
    return ok();
}

bool Hid::OpenNoAccess(const HidInfo& c) {
    Close();
    c_ = c;
    h_ = CreateFileW(c.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    overlapped_ = false;
    return ok();
}

void Hid::Close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

// Waits for an overlapped operation; cancels it on timeout. Never blocks longer than timeoutMs + a little.
static bool Finish(HANDLE h, OVERLAPPED& ov, DWORD& n, int timeoutMs) {
    if (WaitForSingleObject(ov.hEvent, (DWORD)std::max(timeoutMs, 0)) == WAIT_OBJECT_0)
        return GetOverlappedResult(h, &ov, &n, FALSE) != FALSE;
    CancelIoEx(h, &ov);
    // After CancelIoEx the operation completes promptly; bound the wait anyway.
    if (WaitForSingleObject(ov.hEvent, 1000) != WAIT_OBJECT_0) return false;
    return GetOverlappedResult(h, &ov, &n, FALSE) != FALSE;
}

bool Hid::Write(const std::vector<BYTE>& data, int timeoutMs) {
    if (!ok() || !overlapped_ || !c_.outLen || data.empty()) return false;
    std::vector<BYTE> buf(c_.outLen, 0);
    memcpy(buf.data(), data.data(), std::min(buf.size(), data.size()));
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD n = 0;
    bool done = false;
    if (WriteFile(h_, buf.data(), (DWORD)buf.size(), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING)
        done = Finish(h_, ov, n, timeoutMs);
    CloseHandle(ov.hEvent);
    if (!done) {
        // Some receivers only accept output reports through the control pipe.
        done = HidD_SetOutputReport(h_, buf.data(), (ULONG)buf.size()) != FALSE;
    }
    return done;
}

int Hid::Read(std::vector<BYTE>& out, int timeoutMs) {
    if (!ok() || !overlapped_ || !c_.inLen) return -1;
    std::vector<BYTE> tmp(c_.inLen, 0);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;
    DWORD got = 0;
    bool done = false;
    if (ReadFile(h_, tmp.data(), (DWORD)tmp.size(), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING)
        done = Finish(h_, ov, got, timeoutMs);
    CloseHandle(ov.hEvent);
    if (!done || got == 0) return -1;
    size_t off = tmp[0] == 0 ? 1 : 0;
    out.assign(tmp.begin() + off, tmp.begin() + got);
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
