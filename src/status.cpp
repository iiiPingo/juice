// status.txt: a small file Minibar reads to show battery levels in its side menu.
// Rewritten on every reading (atomically), deleted when Juice closes.
//
//   # Juice battery status ...
//   updated=2026-11-27 14:03:05
//   device=headset|82|0|Sony WH-1000XM5
//
// device = type|percent (-1 unknown)|charging (0/1)|name. The name is last so it may contain '|'.
#include "common.h"

const wchar_t* KindWord(Kind k) {
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

static std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}

std::string StatusText(const std::vector<StatusDev>& devs, const std::wstring& updated) {
    std::wstring t = L"# Juice battery status for Minibar. Rewritten on every reading, deleted when Juice closes.\r\n";
    t += L"updated=" + updated + L"\r\n";
    for (const StatusDev& d : devs) {
        std::wstring name = d.name;
        for (wchar_t& c : name)
            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        int pct = d.percent < -1 ? -1 : d.percent > 100 ? 100 : d.percent;
        t += Fmt(L"device=%ls|%d|%d|", KindWord(d.kind), pct, d.charging ? 1 : 0) + name + L"\r\n";
    }
    return Utf8(t);
}

std::wstring StatusPath() { return DataDir() + L"\\status.txt"; }

bool WriteStatus(const std::vector<StatusDev>& devs) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string text = StatusText(devs, Fmt(L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
    std::wstring path = StatusPath(), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
    CloseHandle(h);
    // replace in one step, so a reader never sees half a file
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void ClearStatus() { DeleteFileW(StatusPath().c_str()); }
