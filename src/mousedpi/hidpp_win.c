#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

static int32_t g_connected = 0;
static int32_t g_dpi_x = 800;
static int32_t g_dpi_y = 800;
static int32_t g_lod = 2;
static int32_t g_battery = -1;
static int32_t g_mode = 0;
static int32_t g_win_styled = 0;
static int32_t g_cwd_set = 0;

static void ensure_exe_cwd(void) {
    if (g_cwd_set) return;
    g_cwd_set = 1;
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char *slash = strrchr(buf, '\\');
        if (slash) {
            *slash = '\0';
            SetCurrentDirectoryA(buf);
        }
    }
}

struct FindWinCtx {
    DWORD pid;
    HWND hwnd;
};

static BOOL CALLBACK enum_own_win(HWND hwnd, LPARAM lp) {
    struct FindWinCtx *ctx = (struct FindWinCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL) {
        ctx->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

int32_t mousedpi_setup_window(int32_t logical_w, int32_t logical_h) {
    if (g_win_styled) return 1;
    struct FindWinCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.hwnd = NULL;
    EnumWindows(enum_own_win, (LPARAM)&ctx);
    if (!ctx.hwnd) return 0;

    HWND hwnd = ctx.hwnd;
    // Enable Windows 11 dark title bar & match #0B0C0F caption color
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap_color = RGB(0x0B, 0x0C, 0x0F);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cap_color, sizeof(cap_color));
    COLORREF border_color = RGB(0x22, 0x26, 0x30);
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border_color, sizeof(border_color));

    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(u32, "GetDpiForWindow");
        if (fn) {
            UINT d = fn(hwnd);
            if (d >= 96) dpi = d;
        }
    }
    int cw = (int)((logical_w * (int)dpi + 48) / 96);
    int ch = (int)((logical_h * (int)dpi + 48) / 96);
    RECT rc = { 0, 0, cw, ch };
    DWORD style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, ex_style);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    int x = 160, y = 120;
    if (GetMonitorInfoA(mon, &mi)) {
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
    }
    SetWindowPos(hwnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    g_win_styled = 1;
    return 1;
}

static int str_contains_ci(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nlen && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static HANDLE open_logi_hidpp(void) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    HANDLE found = INVALID_HANDLE_VALUE;

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(info, NULL, &hid_guid, idx, &did); idx++) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &did, NULL, 0, &req, NULL);
        if (req == 0) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, req);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(info, &did, det, req, NULL, NULL)) {
            if (str_contains_ci(det->DevicePath, "vid_046d&pid_c54d") ||
                str_contains_ci(det->DevicePath, "vid_046d&pid_c547") ||
                str_contains_ci(det->DevicePath, "vid_046d&pid_c539")) {
                HANDLE h = CreateFileA(
                    det->DevicePath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    PHIDP_PREPARSED_DATA ppd = NULL;
                    if (HidD_GetPreparsedData(h, &ppd)) {
                        HIDP_CAPS caps;
                        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                            if (caps.UsagePage == 0xFF00 && caps.OutputReportByteLength == 20) {
                                HidD_FreePreparsedData(ppd);
                                HeapFree(GetProcessHeap(), 0, det);
                                found = h;
                                break;
                            }
                        }
                        HidD_FreePreparsedData(ppd);
                    }
                    CloseHandle(h);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static int hidpp_xfer(HANDLE h, uint8_t dev_idx, uint8_t feat_idx, uint8_t fn,
                      const uint8_t *payload, int payload_len, uint8_t resp[20]) {
    uint8_t req[20];
    memset(req, 0, sizeof(req));
    req[0] = 0x11;
    req[1] = dev_idx;
    req[2] = feat_idx;
    req[3] = (uint8_t)((fn << 4) | 0x0A);
    for (int i = 0; i < payload_len && i < 16; i++) req[4 + i] = payload[i];

    OVERLAPPED ovw;
    memset(&ovw, 0, sizeof(ovw));
    ovw.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL wok = WriteFile(h, req, 20, &written, &ovw);
    if (!wok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ovw.hEvent, 300) == WAIT_OBJECT_0) {
            GetOverlappedResult(h, &ovw, &written, FALSE);
            wok = TRUE;
        } else {
            CancelIo(h);
        }
    }
    CloseHandle(ovw.hEvent);
    if (!wok || written != 20) return 0;

    for (int tries = 0; tries < 8; tries++) {
        memset(resp, 0, 20);
        OVERLAPPED ovr;
        memset(&ovr, 0, sizeof(ovr));
        ovr.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        DWORD got = 0;
        BOOL rok = ReadFile(h, resp, 20, &got, &ovr);
        if (!rok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ovr.hEvent, 350) == WAIT_OBJECT_0) {
                GetOverlappedResult(h, &ovr, &got, FALSE);
                rok = TRUE;
            } else {
                CancelIo(h);
            }
        }
        CloseHandle(ovr.hEvent);
        if (!rok || got < 7) return 0;
        if (resp[1] == dev_idx && (resp[2] == feat_idx || resp[2] == 0xFF)) {
            return 1;
        }
    }
    return 0;
}

static uint8_t get_feature_idx(HANDLE h, uint8_t dev_idx, uint16_t fid) {
    uint8_t p[2] = { (uint8_t)(fid >> 8), (uint8_t)(fid & 0xFF) };
    uint8_t r[20];
    if (!hidpp_xfer(h, dev_idx, 0x00, 0x00, p, 2, r)) return 0;
    if (r[2] == 0xFF) return 0;
    return r[4];
}

int32_t mousedpi_read_state(void) {
    ensure_exe_cwd();
    HANDLE h = open_logi_hidpp();
    if (h == INVALID_HANDLE_VALUE) {
        g_connected = 0;
        return 0;
    }
    uint8_t dev = 0x01;
    uint8_t idx_dpi = get_feature_idx(h, dev, 0x2202);
    if (idx_dpi == 0) {
        CloseHandle(h);
        g_connected = 0;
        return 0;
    }
    uint8_t r[20];
    uint8_t p0[1] = { 0x00 };
    if (hidpp_xfer(h, dev, idx_dpi, 5, p0, 1, r) && r[2] != 0xFF) {
        g_dpi_x = ((int32_t)r[5] << 8) | r[6];
        g_dpi_y = ((int32_t)r[9] << 8) | r[10];
        if (g_dpi_y < 100) g_dpi_y = g_dpi_x;
        if (r[13] >= 1 && r[13] <= 3) g_lod = r[13];
        g_connected = 1;
    }
    uint8_t idx_bat = get_feature_idx(h, dev, 0x1004);
    if (idx_bat != 0 && hidpp_xfer(h, dev, idx_bat, 1, NULL, 0, r) && r[2] != 0xFF) {
        g_battery = r[4];
    }
    uint8_t idx_ob = get_feature_idx(h, dev, 0x8100);
    if (idx_ob != 0 && hidpp_xfer(h, dev, idx_ob, 2, NULL, 0, r) && r[2] != 0xFF) {
        g_mode = r[4];
    }
    CloseHandle(h);
    return g_connected;
}

int32_t mousedpi_set_dpi(int32_t x, int32_t y, int32_t lod) {
    if (x < 100) x = 100;
    if (x > 44000) x = 44000;
    if (y < 100) y = 100;
    if (y > 44000) y = 44000;
    if (lod < 1) lod = 1;
    if (lod > 3) lod = 3;

    HANDLE h = open_logi_hidpp();
    if (h == INVALID_HANDLE_VALUE) {
        g_connected = 0;
        return -1;
    }
    uint8_t dev = 0x01;
    uint8_t idx_dpi = get_feature_idx(h, dev, 0x2202);
    if (idx_dpi == 0) {
        CloseHandle(h);
        g_connected = 0;
        return -2;
    }
    uint8_t r[20];
    uint8_t idx_ob = get_feature_idx(h, dev, 0x8100);
    if (idx_ob != 0) {
        uint8_t pm[1] = { 0x02 };
        hidpp_xfer(h, dev, idx_ob, 1, pm, 1, r);
        g_mode = 2;
    }
    uint8_t pd[6] = {
        0x00,
        (uint8_t)((x >> 8) & 0xFF),
        (uint8_t)(x & 0xFF),
        (uint8_t)((y >> 8) & 0xFF),
        (uint8_t)(y & 0xFF),
        (uint8_t)lod
    };
    int ok = hidpp_xfer(h, dev, idx_dpi, 6, pd, 6, r);
    CloseHandle(h);
    if (!ok) return -3;
    if (r[2] == 0xFF) return -(100 + (int32_t)r[5]);
    g_dpi_x = ((int32_t)r[5] << 8) | r[6];
    g_dpi_y = ((int32_t)r[7] << 8) | r[8];
    if (r[9] >= 1 && r[9] <= 3) g_lod = r[9];
    g_connected = 1;
    return 0;
}

int32_t mousedpi_get_dpi_x(void) { return g_dpi_x; }
int32_t mousedpi_get_lod(void) { return g_lod; }
int32_t mousedpi_get_battery(void) { return g_battery; }
int32_t mousedpi_get_mode(void) { return g_mode; }
