// Tests for status.txt (the file Minibar reads). Build and run: tests\run.cmd (exit code 0 = passed).
#include "../src/common.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int wmain() {
    std::vector<StatusDev> v = {
        { L"Headset", Kind::Headset, 82, false },
        { L"Mouse", Kind::Mouse, 46, true },
        { L"Pad | 2\r\nline", Kind::Gamepad, 14, false },
        { L"\u0633\u0645\u0627\u0639\u0629", Kind::Earbuds, -1, false },
        { L"Odd", Kind::Other, 250, false },
    };
    std::string t = StatusText(v, L"2026-11-27 14:03:05");
    Check(t.find("updated=2026-11-27 14:03:05\r\n") != std::string::npos, "updated line");
    Check(t.find("device=headset|82|0|Headset\r\n") != std::string::npos, "headset line");
    Check(t.find("device=mouse|46|1|Mouse\r\n") != std::string::npos, "charging flag");
    Check(t.find("device=controller|14|0|Pad | 2  line\r\n") != std::string::npos, "gamepad is 'controller', newlines in names become spaces");
    Check(t.find("device=earbuds|-1|0|\xD8\xB3\xD9\x85\xD8\xA7\xD8\xB9\xD8\xA9\r\n") != std::string::npos, "UTF-8 name, unknown percent");
    Check(t.find("device=other|100|0|Odd") != std::string::npos, "percent clamped");
    Check(t.compare(0, 3, "\xEF\xBB\xBF") != 0, "no BOM");
    Check(StatusText({}, L"x").find("device=") == std::string::npos, "no devices, no device lines");

    // WriteStatus / ClearStatus in the data folder (next to the test exe when writable)
    Check(WriteStatus(v), "WriteStatus");
    HANDLE h = CreateFileW(StatusPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(h != INVALID_HANDLE_VALUE, "status.txt exists");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    Check(GetFileAttributesW((StatusPath() + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "no .tmp left behind");
    ClearStatus();
    Check(GetFileAttributesW(StatusPath().c_str()) == INVALID_FILE_ATTRIBUTES, "ClearStatus deletes it");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
