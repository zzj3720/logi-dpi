#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_TRAYICON (WM_APP + 42)
#define ID_TRAY_SHOW 1001
#define ID_TRAY_QUIT 1002

static CRITICAL_SECTION g_hid_cs;
static int32_t g_cs_inited = 0;

static int32_t g_connected = 0;
static int32_t g_dpi_x = 800;
static int32_t g_dpi_y = 800;
static int32_t g_lod = 2;
static int32_t g_battery = -1;
static int32_t g_mode = 0;

static int32_t g_has_saved = 0;
static int32_t g_saved_dpi = 800;
static int32_t g_saved_lod = 2;

static int32_t g_win_styled = 0;
static int32_t g_cwd_set = 0;
static int32_t g_watcher_started = 0;
static char g_exe_path[MAX_PATH] = {0};
static char g_cfg_path[MAX_PATH] = {0};

static HWND g_main_hwnd = NULL;
static WNDPROC g_orig_wndproc = NULL;
static NOTIFYICONDATAW g_nid;
static int32_t g_tray_added = 0;

static HANDLE g_wake_event = NULL;
static volatile ULONGLONG g_last_mouse_tick = 0;
static volatile LONG g_wake_burst = 0;
static uint8_t g_cached_idx_dpi = 0;
static uint8_t g_cached_idx_ob = 0;

static void signal_wake_restore(void) {
    InterlockedExchange(&g_wake_burst, 1);
    if (g_wake_event) SetEvent(g_wake_event);
}

static void ensure_init(void) {
    if (!g_cs_inited) {
        InitializeCriticalSection(&g_hid_cs);
        g_cs_inited = 1;
    }
    if (!g_cwd_set) {
        g_cwd_set = 1;
        DWORD len = GetModuleFileNameA(NULL, g_exe_path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            char dir[MAX_PATH];
            strncpy(dir, g_exe_path, MAX_PATH - 1);
            dir[MAX_PATH - 1] = '\0';
            char *slash = strrchr(dir, '\\');
            if (slash) {
                *slash = '\0';
                SetCurrentDirectoryA(dir);
                snprintf(g_cfg_path, MAX_PATH, "%s\\config.ini", dir);
            }
        }
    }
}

static void save_config(int32_t dpi, int32_t lod) {
    ensure_init();
    g_has_saved = 1;
    g_saved_dpi = dpi;
    g_saved_lod = lod;
    if (g_cfg_path[0]) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", dpi);
        WritePrivateProfileStringA("mouse", "dpi", buf, g_cfg_path);
        snprintf(buf, sizeof(buf), "%d", lod);
        WritePrivateProfileStringA("mouse", "lod", buf, g_cfg_path);
    }
}

static void update_tray_tip(void) {
    if (!g_tray_added || !g_main_hwnd) return;
    swprintf(g_nid.szTip, 128, L"PRO X2 SUPERSTRIKE - %d DPI", g_dpi_x);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void remove_tray_icon(void) {
    if (g_tray_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray_added = 0;
    }
}

static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG prev = g_last_mouse_tick;
        g_last_mouse_tick = now;
        if (prev != 0 && (now - prev) >= 5000 && g_has_saved) {
            signal_wake_restore();
        }
        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
    }
    if (msg == WM_POWERBROADCAST) {
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            if (g_has_saved) signal_wake_restore();
        }
        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
    }
    if (msg == WM_CLOSE) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    if (msg == WM_TRAYICON) {
        UINT ev = (UINT)(lp & 0xFFFF);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;
        }
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (menu) {
                AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"\x6253\x5F00 MouseDPI");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"\x9000\x51FA");
                SetForegroundWindow(hwnd);
                UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(menu);
                if (cmd == ID_TRAY_SHOW) {
                    ShowWindow(hwnd, SW_SHOW);
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                } else if (cmd == ID_TRAY_QUIT) {
                    remove_tray_icon();
                    ExitProcess(0);
                }
            }
            return 0;
        }
    }
    if (msg == WM_DESTROY) {
        remove_tray_icon();
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

static void add_tray_icon(HWND hwnd) {
    if (g_tray_added) return;
    HINSTANCE hInst = GetModuleHandleW(NULL);
    HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    HICON hBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (!hSmall) hSmall = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    if (!hBig) hBig = hSmall;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = hSmall;
    swprintf(g_nid.szTip, 128, L"PRO X2 SUPERSTRIKE - %d DPI", g_dpi_x);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_tray_added = 1;
    }
}

static int has_tray_arg(void) {
    const char *cmd = GetCommandLineA();
    if (!cmd) return 0;
    return strstr(cmd, "--tray") != NULL;
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

static HANDLE open_logi_hidpp_caps(DWORD access, USHORT want_in_len, USHORT want_out_len) {
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
                    access,
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
                            int match_in = (want_in_len == 0 || caps.InputReportByteLength == want_in_len);
                            int match_out = (want_out_len == 0 || caps.OutputReportByteLength == want_out_len);
                            if (caps.UsagePage == 0xFF00 && match_in && match_out) {
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

static HANDLE open_logi_hidpp(void) {
    return open_logi_hidpp_caps(GENERIC_READ | GENERIC_WRITE, 0, 20);
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

static int32_t read_state_locked(void) {
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
    g_cached_idx_dpi = idx_dpi;
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
    if (idx_ob != 0) g_cached_idx_ob = idx_ob;
    if (idx_ob != 0 && hidpp_xfer(h, dev, idx_ob, 2, NULL, 0, r) && r[2] != 0xFF) {
        g_mode = r[4];
    }
    CloseHandle(h);
    return g_connected;
}

static int32_t set_dpi_locked(int32_t x, int32_t y, int32_t lod) {
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

static int32_t ensure_dpi_fast_locked(int force_write) {
    HANDLE h = open_logi_hidpp();
    if (h == INVALID_HANDLE_VALUE) {
        g_connected = 0;
        g_cached_idx_dpi = 0;
        g_cached_idx_ob = 0;
        return 0;
    }
    uint8_t dev = 0x01;
    uint8_t idx_dpi = g_cached_idx_dpi;
    if (idx_dpi == 0) {
        idx_dpi = get_feature_idx(h, dev, 0x2202);
        g_cached_idx_dpi = idx_dpi;
    }
    if (idx_dpi == 0) {
        CloseHandle(h);
        g_connected = 0;
        return 0;
    }
    uint8_t r[20];
    int need_write = force_write;
    if (!need_write) {
        uint8_t p0[1] = { 0x00 };
        if (hidpp_xfer(h, dev, idx_dpi, 5, p0, 1, r) && r[2] != 0xFF) {
            int32_t cx = ((int32_t)r[5] << 8) | r[6];
            int32_t cy = ((int32_t)r[9] << 8) | r[10];
            if (cy < 100) cy = cx;
            int32_t clod = (r[13] >= 1 && r[13] <= 3) ? r[13] : g_lod;
            g_dpi_x = cx;
            g_dpi_y = cy;
            g_lod = clod;
            g_connected = 1;
            if (cx != g_saved_dpi || cy != g_saved_dpi || clod != g_saved_lod) {
                need_write = 1;
            }
        } else {
            g_cached_idx_dpi = 0;
            CloseHandle(h);
            g_connected = 0;
            return 0;
        }
    }
    if (need_write) {
        uint8_t idx_ob = g_cached_idx_ob;
        if (idx_ob == 0) {
            idx_ob = get_feature_idx(h, dev, 0x8100);
            g_cached_idx_ob = idx_ob;
        }
        if (idx_ob != 0) {
            uint8_t pm[1] = { 0x02 };
            hidpp_xfer(h, dev, idx_ob, 1, pm, 1, r);
            g_mode = 2;
        }
        int32_t x = g_saved_dpi;
        int32_t lod = g_saved_lod;
        uint8_t pd[6] = {
            0x00,
            (uint8_t)((x >> 8) & 0xFF),
            (uint8_t)(x & 0xFF),
            (uint8_t)((x >> 8) & 0xFF),
            (uint8_t)(x & 0xFF),
            (uint8_t)lod
        };
        if (hidpp_xfer(h, dev, idx_dpi, 6, pd, 6, r) && r[2] != 0xFF) {
            g_dpi_x = ((int32_t)r[5] << 8) | r[6];
            g_dpi_y = ((int32_t)r[7] << 8) | r[8];
            if (r[9] >= 1 && r[9] <= 3) g_lod = r[9];
            g_connected = 1;
        } else {
            g_cached_idx_dpi = 0;
            g_cached_idx_ob = 0;
            CloseHandle(h);
            return 0;
        }
    }
    CloseHandle(h);
    return 1;
}

static DWORD WINAPI hid_notify_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        DWORD want_len = 7;
        HANDLE h = open_logi_hidpp_caps(GENERIC_READ, 7, 0);
        if (h == INVALID_HANDLE_VALUE) {
            want_len = 20;
            h = open_logi_hidpp_caps(GENERIC_READ, 20, 0);
        }
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ev) {
            CloseHandle(h);
            Sleep(2000);
            continue;
        }
        for (;;) {
            uint8_t buf[20];
            memset(buf, 0, sizeof(buf));
            OVERLAPPED ov;
            memset(&ov, 0, sizeof(ov));
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD got = 0;
            BOOL ok = ReadFile(h, buf, want_len, &got, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    DWORD w = WaitForSingleObject(ev, INFINITE);
                    if (w != WAIT_OBJECT_0 || !GetOverlappedResult(h, &ov, &got, FALSE)) {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (got >= 7 && (buf[0] == 0x10 || buf[0] == 0x11) && buf[1] == 0x01) {
                /* 0x41 = Wireless Device Connection notification (bit 6 of buf[4] == 0 means link established / woke up) */
                if (buf[2] == 0x41 && ((buf[4] & 0x40) == 0)) {
                    if (g_has_saved) signal_wake_restore();
                }
            }
        }
        CloseHandle(ev);
        CloseHandle(h);
        Sleep(1000);
    }
    return 0;
}

static DWORD WINAPI wake_watcher_thread(LPVOID unused) {
    (void)unused;
    static const DWORD retry_delays[3] = { 40, 120, 300 };
    for (;;) {
        DWORD wait_rc = g_wake_event ? WaitForSingleObject(g_wake_event, 2000) : (Sleep(2000), WAIT_TIMEOUT);
        if (!g_has_saved) continue;

        int burst = (wait_rc == WAIT_OBJECT_0) || (InterlockedExchange(&g_wake_burst, 0) != 0);
        if (!burst) {
            /* Gentle fallback only while mouse was recently active, so we do not keep a sleeping mouse awake */
            ULONGLONG now = GetTickCount64();
            ULONGLONG last = g_last_mouse_tick;
            if (last != 0 && (now - last) > 15000) {
                continue;
            }
            EnterCriticalSection(&g_hid_cs);
            ensure_dpi_fast_locked(0);
            LeaveCriticalSection(&g_hid_cs);
            continue;
        }

        /* Instant wake restore with fast retries in case radio handshake is still completing */
        int restored = 0;
        for (int attempt = 0; attempt < 4; attempt++) {
            EnterCriticalSection(&g_hid_cs);
            restored = ensure_dpi_fast_locked(0);
            LeaveCriticalSection(&g_hid_cs);
            if (restored) break;
            if (attempt < 3) Sleep(retry_delays[attempt]);
        }
        if (restored) {
            update_tray_tip();
        }
    }
    return 0;
}

static void start_watcher_once(void) {
    if (g_watcher_started) return;
    g_watcher_started = 1;
    if (!g_wake_event) {
        g_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    g_last_mouse_tick = GetTickCount64();
    HANDLE th1 = CreateThread(NULL, 0, wake_watcher_thread, NULL, 0, NULL);
    if (th1) CloseHandle(th1);
    HANDLE th2 = CreateThread(NULL, 0, hid_notify_thread, NULL, 0, NULL);
    if (th2) CloseHandle(th2);
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
    ensure_init();
    if (g_win_styled) return 1;
    struct FindWinCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.hwnd = NULL;
    EnumWindows(enum_own_win, (LPARAM)&ctx);
    if (!ctx.hwnd) return 0;

    HWND hwnd = ctx.hwnd;
    g_main_hwnd = hwnd;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF cap_color = RGB(0x0B, 0x0C, 0x0F);
    DwmSetWindowAttribute(hwnd, 35, &cap_color, sizeof(cap_color));
    COLORREF border_color = RGB(0x22, 0x26, 0x30);
    DwmSetWindowAttribute(hwnd, 34, &border_color, sizeof(border_color));

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

    g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)tray_wndproc);
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02; /* Generic Desktop Mouse */
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
    add_tray_icon(hwnd);
    if (has_tray_arg()) {
        ShowWindow(hwnd, SW_HIDE);
    }
    start_watcher_once();
    g_win_styled = 1;
    return 1;
}

int32_t mousedpi_init_and_restore(void) {
    ensure_init();
    int32_t cfg_dpi = (int32_t)GetPrivateProfileIntA("mouse", "dpi", 0, g_cfg_path);
    int32_t cfg_lod = (int32_t)GetPrivateProfileIntA("mouse", "lod", 2, g_cfg_path);
    if (cfg_lod < 1 || cfg_lod > 3) cfg_lod = 2;

    EnterCriticalSection(&g_hid_cs);
    int32_t ok = read_state_locked();
    if (cfg_dpi >= 100 && cfg_dpi <= 44000) {
        g_has_saved = 1;
        g_saved_dpi = cfg_dpi;
        g_saved_lod = cfg_lod;
        if (ok && (g_dpi_x != cfg_dpi || g_dpi_y != cfg_dpi || g_lod != cfg_lod)) {
            set_dpi_locked(cfg_dpi, cfg_dpi, cfg_lod);
        }
    } else if (ok) {
        save_config(g_dpi_x, g_lod);
    }
    LeaveCriticalSection(&g_hid_cs);
    update_tray_tip();
    start_watcher_once();
    return ok;
}

int32_t mousedpi_read_state(void) {
    ensure_init();
    EnterCriticalSection(&g_hid_cs);
    int32_t ok = read_state_locked();
    LeaveCriticalSection(&g_hid_cs);
    update_tray_tip();
    return ok;
}

int32_t mousedpi_set_dpi(int32_t x, int32_t y, int32_t lod) {
    ensure_init();
    EnterCriticalSection(&g_hid_cs);
    int32_t rc = set_dpi_locked(x, y, lod);
    if (rc == 0) {
        save_config(g_dpi_x, g_lod);
    }
    LeaveCriticalSection(&g_hid_cs);
    update_tray_tip();
    return rc;
}

int32_t mousedpi_get_autostart(void) {
    HKEY hk = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return 0;
    }
    char val[MAX_PATH * 2];
    DWORD sz = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExA(hk, "LogiDPI", NULL, &type, (LPBYTE)val, &sz);
    RegCloseKey(hk);
    return (st == ERROR_SUCCESS && type == REG_SZ && sz > 1) ? 1 : 0;
}

int32_t mousedpi_set_autostart(int32_t enable) {
    ensure_init();
    HKEY hk = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) {
        return 0;
    }
    if (enable) {
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof(cmd), "\"%s\" --tray", g_exe_path);
        RegSetValueExA(hk, "LogiDPI", 0, REG_SZ, (const BYTE *)cmd, (DWORD)(strlen(cmd) + 1));
    } else {
        RegDeleteValueA(hk, "LogiDPI");
    }
    RegCloseKey(hk);
    return mousedpi_get_autostart();
}

int32_t mousedpi_get_dpi_x(void) { return g_dpi_x; }
int32_t mousedpi_get_lod(void) { return g_lod; }
int32_t mousedpi_get_battery(void) { return g_battery; }
int32_t mousedpi_get_mode(void) { return g_mode; }
