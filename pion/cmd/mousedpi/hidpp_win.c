#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <powrprof.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "powrprof.lib")


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

static int32_t g_cwd_set = 0;
static int32_t g_watcher_started = 0;
static char g_exe_path[MAX_PATH] = {0};
static char g_cfg_path[MAX_PATH] = {0};


static HANDLE g_wake_event = NULL;
static volatile ULONGLONG g_last_mouse_tick = 0;
static volatile LONG g_wake_burst = 0;
static uint8_t g_cached_idx_dpi = 0;
static uint8_t g_cached_idx_ob = 0;

static void signal_wake_restore(void) {
    InterlockedExchange(&g_wake_burst, 1);
    if (g_wake_event) SetEvent(g_wake_event);
}

static HANDLE g_refresh_event = NULL;
static CRITICAL_SECTION g_want_cs;

/* First called on the window's thread (mousedpi_peek_config, before the mouse is read on a thread of its own): what
 * the threads share is made here, once. */
static void ensure_init(void) {
    if (!g_cs_inited) {
        InitializeCriticalSection(&g_hid_cs);
        InitializeCriticalSection(&g_want_cs);
        g_refresh_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
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

/* Started with Windows (the Run key's --tray): kept in the tray till brought back. */
int32_t mousedpi_started_hidden(void) {
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
    }
    return 0;
}

/* The mouse's raw input (the wake guard: moved again after 5 s still), on a message-only window of its own thread: a
 * high-polling mouse sends thousands of WM_INPUT a second, which must never queue on the UI's thread. */
static LRESULT CALLBACK raw_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG prev = g_last_mouse_tick;
        g_last_mouse_tick = now;
        if (prev != 0 && (now - prev) >= 5000 && g_has_saved) {
            signal_wake_restore();
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI raw_input_thread(LPVOID unused) {
    (void)unused;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = raw_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MouseDPIRawInput";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!h) return 0;
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02; /* Generic Desktop Mouse */
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = h;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

/* The window's view of the mouse is read here, off its thread (HID++ answers can take hundreds of ms, more while the
 * receiver is busy): every 30 s, or at once when asked (mousedpi_request_refresh). g_state_rev counts the reads done,
 * so the window takes the cached values (mousedpi_get_*) only when there is something new. */
static volatile LONG g_state_rev = 0;
static SRWLOCK g_rev_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_rev_cv = CONDITION_VARIABLE_INIT;
static int32_t g_rev_stop = 0;

/* A read done: whoever waits for one (mousedpi_wait_rev) goes on. */
static void bump_rev(void) {
    AcquireSRWLockExclusive(&g_rev_lock);
    InterlockedIncrement(&g_state_rev);
    ReleaseSRWLockExclusive(&g_rev_lock);
    WakeAllConditionVariable(&g_rev_cv);
}

/* Writes the window asks for (a slider dragged asks many), done here off its thread: only the newest one wanted is
 * written, the ones it replaced are dropped. */
static int32_t g_want_pending = 0, g_want_dpi = 0, g_want_lod = 0;

static DWORD WINAPI refresher_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        DWORD w = WaitForSingleObject(g_refresh_event, 30000);
        EnterCriticalSection(&g_want_cs);
        int32_t pending = g_want_pending, dpi = g_want_dpi, lod = g_want_lod;
        g_want_pending = 0;
        LeaveCriticalSection(&g_want_cs);
        EnterCriticalSection(&g_hid_cs);
        if (pending) {
            if (set_dpi_locked(dpi, dpi, lod) == 0) save_config(g_dpi_x, g_lod);
            else read_state_locked();
        } else {
            (void)w;
            read_state_locked();
        }
        LeaveCriticalSection(&g_hid_cs);
        bump_rev();
        }
    return 0;
}

/* The PC woke (the system's suspend/resume notification, no window needed): the DPI put back, as when the mouse wakes. */
static ULONG CALLBACK power_said(PVOID ctx, ULONG type, PVOID setting) {
    (void)ctx;
    (void)setting;
    if ((type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) && g_has_saved) signal_wake_restore();
    return 0;
}

static void start_watcher_once(void) {
    if (InterlockedCompareExchange((volatile LONG *)&g_watcher_started, 1, 0) != 0) return;
    g_last_mouse_tick = GetTickCount64();
    static DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS power = {power_said, NULL};
    HPOWERNOTIFY ignored = NULL;
    PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &power, &ignored);
    HANDLE th1 = CreateThread(NULL, 0, wake_watcher_thread, NULL, 0, NULL);
    if (th1) CloseHandle(th1);
    HANDLE th2 = CreateThread(NULL, 0, hid_notify_thread, NULL, 0, NULL);
    if (th2) CloseHandle(th2);
    HANDLE th3 = CreateThread(NULL, 0, refresher_thread, NULL, 0, NULL);
    if (th3) CloseHandle(th3);
    HANDLE th4 = CreateThread(NULL, 0, raw_input_thread, NULL, 0, NULL);
    if (th4) CloseHandle(th4);
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
    start_watcher_once();
    return ok;
}

int32_t mousedpi_read_state(void) {
    ensure_init();
    EnterCriticalSection(&g_hid_cs);
    int32_t ok = read_state_locked();
    LeaveCriticalSection(&g_hid_cs);
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

/* Ask the refresher thread to read the mouse now (returns at once). */
void mousedpi_request_refresh(void) {
    ensure_init();
    start_watcher_once();
    if (g_refresh_event) SetEvent(g_refresh_event);
}

/* Ask for DPI (both axes) and LOD to be written (returns at once; the refresher thread writes the newest asked). */
void mousedpi_request_set(int32_t dpi, int32_t lod) {
    ensure_init();
    start_watcher_once();
    EnterCriticalSection(&g_want_cs);
    g_want_pending = 1;
    g_want_dpi = dpi;
    g_want_lod = lod;
    LeaveCriticalSection(&g_want_cs);
    SetEvent(g_refresh_event);
}

int32_t mousedpi_state_rev(void) { return (int32_t)g_state_rev; }

/* Blocks till a read newer than `seen` is done (its rev), or -1 once mousedpi_stop_waits is called: the window's
 * reader thread waits here, not the window. */
int32_t mousedpi_wait_rev(int32_t seen) {
    AcquireSRWLockExclusive(&g_rev_lock);
    while ((int32_t)g_state_rev == seen && !g_rev_stop) SleepConditionVariableSRW(&g_rev_cv, &g_rev_lock, INFINITE, 0);
    int32_t r = g_rev_stop ? -1 : (int32_t)g_state_rev;
    ReleaseSRWLockExclusive(&g_rev_lock);
    return r;
}

void mousedpi_stop_waits(void) {
    AcquireSRWLockExclusive(&g_rev_lock);
    g_rev_stop = 1;
    ReleaseSRWLockExclusive(&g_rev_lock);
    WakeAllConditionVariable(&g_rev_cv);
}

/* What config.ini says the mouse was last set to (0: dpi, 1: lod), read without the mouse: shown till it answers. */
int32_t mousedpi_peek_config(int32_t which) {
    ensure_init();
    if (which == 0) return (int32_t)GetPrivateProfileIntA("mouse", "dpi", 800, g_cfg_path);
    int32_t lod = (int32_t)GetPrivateProfileIntA("mouse", "lod", 2, g_cfg_path);
    return (lod < 1 || lod > 3) ? 2 : lod;
}
int32_t mousedpi_get_connected(void) { return g_connected; }
int32_t mousedpi_get_dpi_x(void) { return g_dpi_x; }
int32_t mousedpi_get_lod(void) { return g_lod; }
int32_t mousedpi_get_battery(void) { return g_battery; }
int32_t mousedpi_get_mode(void) { return g_mode; }
