#include "common.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <mutex>

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return buf;
}

static bool DirWritable(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.juice_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::wstring DataDir() {
    static std::wstring dir;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring exe = ExePath();
        std::wstring exeDir = exe.substr(0, exe.find_last_of(L"\\/"));
        if (!exeDir.empty() && DirWritable(exeDir)) {
            dir = exeDir;
            return;
        }
        wchar_t* appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
            dir = std::wstring(appdata) + L"\\Juice";
            CoTaskMemFree(appdata);
        } else {
            dir = exeDir;
        }
        CreateDirectoryW(dir.c_str(), nullptr);
    });
    return dir;
}

std::wstring Hex4(unsigned v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v & 0xFFFF);
    return b;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 2048, fmt, ap);
    va_end(ap);
    return buf;
}

bool IsJunkName(const std::wstring& name) {
    std::wstring t;
    for (wchar_t c : name) {
        if (c < 0x20 || c == 0x7F || c == 0xFFFD) return true;
        if (c != L' ') t += (wchar_t)towupper(c);
    }
    if (t.empty()) return true;
    if (t.size() < 3) return false;
    return t.find_first_not_of(t[0]) == std::wstring::npos;  // one character repeated
}

void Log(const wchar_t* fmt, ...) {
    static std::mutex m;
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 2048, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(m);
    std::wstring path = DataDir() + L"\\juice.log";
    // Keep the log small: when it passes 512 KB, the old one becomes juice.old.log.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024) {
        std::wstring old = DataDir() + L"\\juice.old.log";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2200];
    int n = swprintf(line, 2200, L"%04d-%02d-%02d %02d:%02d:%02d  %ls\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, msg);
    if (n > 0) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
        std::string utf8(bytes, 0);
        WideCharToMultiByte(CP_UTF8, 0, line, n, utf8.data(), bytes, nullptr, nullptr);
        DWORD w;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    }
    CloseHandle(h);
}

ULONGLONG AwakeMs() {
    ULONGLONG t = 0;
    QueryUnbiasedInterruptTime(&t);
    return t / 10000;
}
