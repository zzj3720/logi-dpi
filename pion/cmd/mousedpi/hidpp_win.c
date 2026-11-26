#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <powrprof.h>

#include "hidpp_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "shell32.lib")
#endif

struct hidpp_dev {
    HANDLE h;
    USHORT in_len;
    USHORT out_len;
};

static int             g_win_inited = 0;
static char            g_exe_path[MAX_PATH] = {0};
static char            g_exe_dir[MAX_PATH]  = {0};
static char            g_cfg_path[MAX_PATH] = {0};

static volatile LONG64 g_last_mouse_tick = 0;
static int             g_win_watchers_started = 0;
static HPOWERNOTIFY    g_power_notify = NULL;
static HANDLE          g_raw_thread = NULL;
static volatile HWND   g_raw_hwnd = NULL;

void hidpp_plat_init(void) {
    if (g_win_inited) return;
    g_win_inited = 1;
    DWORD len = GetModuleFileNameA(NULL, g_exe_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        strncpy(g_exe_dir, g_exe_path, MAX_PATH - 1);
        g_exe_dir[MAX_PATH - 1] = '\0';
        char *slash = strrchr(g_exe_dir, '\\');
        if (slash) {
            *slash = '\0';
            SetCurrentDirectoryA(g_exe_dir);
            snprintf(g_cfg_path, MAX_PATH, "%s\\config.ini", g_exe_dir);
        }
    }
    InterlockedExchange64(&g_last_mouse_tick, (LONG64)GetTickCount64());
}

uint64_t hidpp_plat_now_ms(void) {
    return (uint64_t)GetTickCount64();
}

void hidpp_plat_sleep_ms(uint32_t ms) {
    Sleep((DWORD)ms);
}

uint64_t hidpp_plat_mouse_idle_ms(void) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = (ULONGLONG)InterlockedCompareExchange64(&g_last_mouse_tick, 0, 0);
    if (last == 0 || now < last) return 0;
    return (uint64_t)(now - last);
}

int hidpp_plat_needs_notify_thread(void) {
    return 1;
}

/* Exact argv token match for --tray via CommandLineToArgvW. */
int32_t hidpp_plat_started_hidden(void) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    int32_t found = 0;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--tray") == 0) {
            found = 1;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

int32_t hidpp_plat_can_hide_to_tray(void) {
    return 1;
}

static int parse_hex4_after(const char *hay, const char *prefix, uint16_t *out) {
    size_t plen = strlen(prefix);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < plen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)prefix[i])) {
            i++;
        }
        if (i == plen) {
            unsigned int val = 0;
            for (int d = 0; d < 4; d++) {
                char c = p[plen + d];
                if (!isxdigit((unsigned char)c)) return 0;
                val <<= 4;
                if (c >= '0' && c <= '9') val |= (unsigned int)(c - '0');
                else if (c >= 'a' && c <= 'f') val |= (unsigned int)(c - 'a' + 10);
                else val |= (unsigned int)(c - 'A' + 10);
            }
            *out = (uint16_t)val;
            return 1;
        }
    }
    return 0;
}

static int cmp_endpoint_kind(const void *a, const void *b) {
    const hidpp_endpoint_info_t *ea = (const hidpp_endpoint_info_t *)a;
    const hidpp_endpoint_info_t *eb = (const hidpp_endpoint_info_t *)b;
    if (ea->kind < eb->kind) return -1;
    if (ea->kind > eb->kind) return 1;
    return 0;
}

int hidpp_plat_enumerate(hidpp_endpoint_info_t *out, int max_count, int *out_perm_denied) {
    if (out_perm_denied) *out_perm_denied = 0;
    if (!out || max_count <= 0) return 0;

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    int count = 0;

    for (DWORD idx = 0; count < max_count && SetupDiEnumDeviceInterfaces(info, NULL, &hid_guid, idx, &did); idx++) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &did, NULL, 0, &req, NULL);
        if (req == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, req);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (SetupDiGetDeviceInterfaceDetailA(info, &did, det, req, NULL, NULL)) {
            uint16_t vid = 0, pid = 0;
            if (parse_hex4_after(det->DevicePath, "vid_", &vid) &&
                vid == HIDPP_VID_LOGITECH &&
                parse_hex4_after(det->DevicePath, "pid_", &pid)) {
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
                        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS &&
                            caps.UsagePage == HIDPP_USAGE_PAGE_VENDOR &&
                            caps.OutputReportByteLength == HIDPP_LONG_LEN) {
                            hidpp_endpoint_info_t *ep = &out[count++];
                            memset(ep, 0, sizeof(*ep));
                            snprintf(ep->id, sizeof(ep->id), "%s", det->DevicePath);
                            ep->vid = vid;
                            ep->pid = pid;
                            ep->kind = hidpp_classify_pid(pid);
                        }
                        HidD_FreePreparsedData(ppd);
                    }
                    CloseHandle(h);
                } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                    if (out_perm_denied) *out_perm_denied = 1;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }

    SetupDiDestroyDeviceInfoList(info);
    if (count > 1) {
        qsort(out, (size_t)count, sizeof(hidpp_endpoint_info_t), cmp_endpoint_kind);
    }
    return count;
}

static HANDLE open_notify_short_handle(USHORT want_in_len, USHORT *out_in_len) {
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
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, req);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(info, &did, det, req, NULL, NULL)) {
            uint16_t vid = 0;
            if (parse_hex4_after(det->DevicePath, "vid_", &vid) && vid == HIDPP_VID_LOGITECH) {
                HANDLE h = CreateFileA(
                    det->DevicePath,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    PHIDP_PREPARSED_DATA ppd = NULL;
                    if (HidD_GetPreparsedData(h, &ppd)) {
                        HIDP_CAPS caps;
                        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS &&
                            caps.UsagePage == HIDPP_USAGE_PAGE_VENDOR &&
                            caps.InputReportByteLength == want_in_len) {
                            *out_in_len = caps.InputReportByteLength;
                            HidD_FreePreparsedData(ppd);
                            HeapFree(GetProcessHeap(), 0, det);
                            found = h;
                            break;
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

hidpp_dev_t *hidpp_plat_open(const char *endpoint_id, int notify_only, int *out_perm_denied) {
    if (out_perm_denied) *out_perm_denied = 0;

    HANDLE h = INVALID_HANDLE_VALUE;
    USHORT in_len = HIDPP_LONG_LEN;
    USHORT out_len = HIDPP_LONG_LEN;

    if (notify_only) {
        /* Prefer the 7-byte short HID++ TLC on Windows where 0x41 wake notifications arrive. */
        h = open_notify_short_handle(HIDPP_SHORT_LEN, &in_len);
        if (h == INVALID_HANDLE_VALUE && endpoint_id && endpoint_id[0]) {
            h = CreateFileA(
                endpoint_id,
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                NULL);
        }
    } else if (endpoint_id && endpoint_id[0]) {
        h = CreateFileA(
            endpoint_id,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);
    }

    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED && out_perm_denied) {
            *out_perm_denied = 1;
        }
        return NULL;
    }

    PHIDP_PREPARSED_DATA ppd = NULL;
    if (HidD_GetPreparsedData(h, &ppd)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.InputReportByteLength > 0) in_len = caps.InputReportByteLength;
            if (caps.OutputReportByteLength > 0) out_len = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(ppd);
    }

    hidpp_dev_t *dev = (hidpp_dev_t *)malloc(sizeof(*dev));
    if (!dev) {
        CloseHandle(h);
        return NULL;
    }
    dev->h = h;
    dev->in_len = in_len;
    dev->out_len = out_len;
    return dev;
}

void hidpp_plat_close(hidpp_dev_t *dev) {
    if (!dev) return;
    if (dev->h != INVALID_HANDLE_VALUE) CloseHandle(dev->h);
    free(dev);
}

void hidpp_plat_drain(hidpp_dev_t *dev) {
    if (!dev || dev->h == INVALID_HANDLE_VALUE) return;
    (void)HidD_FlushQueue(dev->h);
}

int hidpp_plat_write(hidpp_dev_t *dev, const uint8_t *buf, size_t len) {
    if (!dev || dev->h == INVALID_HANDLE_VALUE || !buf || len == 0) return -1;

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    DWORD written = 0;
    BOOL ok = WriteFile(dev->h, buf, (DWORD)len, &written, &ov);
    if (!ok) {
        if (GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 300);
            if (w == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(dev->h, &ov, &written, FALSE);
            } else {
                /* Must wait for kernel I/O completion before releasing stack OVERLAPPED & event. */
                CancelIoEx(dev->h, &ov);
                (void)GetOverlappedResult(dev->h, &ov, &written, TRUE);
                ok = FALSE;
            }
        }
    }
    CloseHandle(ov.hEvent);
    return ok ? (int)written : -1;
}

int hidpp_plat_read(hidpp_dev_t *dev, uint8_t *buf, size_t cap, int timeout_ms) {
    if (!dev || dev->h == INVALID_HANDLE_VALUE || !buf || cap == 0) return -1;

    DWORD want = (DWORD)dev->in_len;
    if (want == 0 || want > (DWORD)cap) want = (DWORD)cap;

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    DWORD got = 0;
    BOOL ok = ReadFile(dev->h, buf, want, &got, &ov);
    int rc = -1;
    if (ok) {
        rc = (int)got;
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
            if (w == WAIT_OBJECT_0) {
                if (GetOverlappedResult(dev->h, &ov, &got, FALSE)) {
                    rc = (int)got;
                } else {
                    rc = -1;
                }
            } else if (w == WAIT_TIMEOUT) {
                /* Must wait for cancelled I/O to complete before freeing stack OVERLAPPED & event. */
                CancelIoEx(dev->h, &ov);
                if (GetOverlappedResult(dev->h, &ov, &got, TRUE) && got > 0) {
                    rc = (int)got;
                } else {
                    rc = 0;
                }
            } else {
                CancelIoEx(dev->h, &ov);
                (void)GetOverlappedResult(dev->h, &ov, &got, TRUE);
                rc = -1;
            }
        } else {
            rc = -1;
        }
    }
    CloseHandle(ov.hEvent);
    return rc;
}

/* ---- Power suspend/resume + WM_INPUT wake guard ---- */

static ULONG CALLBACK win_power_cb(PVOID ctx, ULONG type, PVOID setting) {
    (void)ctx;
    (void)setting;
    if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
        hidpp_core_signal_wake();
    }
    return 0;
}

static LRESULT CALLBACK raw_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG prev = (ULONGLONG)InterlockedExchange64(&g_last_mouse_tick, (LONG64)now);
        if (prev != 0 && now >= prev && (now - prev) >= 5000) {
            hidpp_core_signal_wake();
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
    g_raw_hwnd = h;

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02; /* Generic Desktop Mouse */
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = h;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        DispatchMessageW(&m);
    }
    DestroyWindow(h);
    g_raw_hwnd = NULL;
    return 0;
}

void hidpp_plat_start_watchers(void) {
    if (g_win_watchers_started) return;
    g_win_watchers_started = 1;
    InterlockedExchange64(&g_last_mouse_tick, (LONG64)GetTickCount64());

    static DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS power = { win_power_cb, NULL };
    PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &power, &g_power_notify);
    g_raw_thread = CreateThread(NULL, 0, raw_input_thread, NULL, 0, NULL);
}

void hidpp_plat_stop_watchers(void) {
    if (!g_win_watchers_started) return;
    g_win_watchers_started = 0;
    if (g_power_notify) {
        PowerUnregisterSuspendResumeNotification(g_power_notify);
        g_power_notify = NULL;
    }
    if (g_raw_hwnd) {
        PostMessageW(g_raw_hwnd, WM_QUIT, 0, 0);
    }
    if (g_raw_thread) {
        WaitForSingleObject(g_raw_thread, 1000);
        CloseHandle(g_raw_thread);
        g_raw_thread = NULL;
    }
}

/* ---- Config (native GetPrivateProfileIntA / WritePrivateProfileStringA) ---- */

int hidpp_plat_config_read(int32_t *out_dpi, int32_t *out_lod) {
    hidpp_plat_init();
    if (!g_cfg_path[0]) return 0;
    int32_t dpi = (int32_t)GetPrivateProfileIntA("mouse", "dpi", 0, g_cfg_path);
    int32_t lod = (int32_t)GetPrivateProfileIntA("mouse", "lod", HIDPP_DEFAULT_LOD, g_cfg_path);
    if (dpi < HIDPP_MIN_DPI || dpi > HIDPP_MAX_DPI) return 0;
    if (lod < 1 || lod > 3) lod = HIDPP_DEFAULT_LOD;
    if (out_dpi) *out_dpi = dpi;
    if (out_lod) *out_lod = lod;
    return 1;
}

int hidpp_plat_config_write(int32_t dpi, int32_t lod) {
    hidpp_plat_init();
    if (!g_cfg_path[0]) return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)dpi);
    BOOL ok1 = WritePrivateProfileStringA("mouse", "dpi", buf, g_cfg_path);
    snprintf(buf, sizeof(buf), "%d", (int)lod);
    BOOL ok2 = WritePrivateProfileStringA("mouse", "lod", buf, g_cfg_path);
    return (ok1 && ok2) ? 1 : 0;
}

/* ---- Autostart (HKCU\...\Run\LogiDPI) ---- */

int32_t hidpp_plat_get_autostart(void) {
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

int32_t hidpp_plat_set_autostart(int32_t enable) {
    hidpp_plat_init();
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
    return hidpp_plat_get_autostart();
}

/* ---- Picture path ---- */

static char *dup_str_win(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

char *hidpp_plat_picture_path_dup(void) {
    hidpp_plat_init();
    char cand[3][MAX_PATH];
    snprintf(cand[0], MAX_PATH, "%s\\mouse.png", g_exe_dir);
    snprintf(cand[1], MAX_PATH, "%s\\assets\\mouse.png", g_exe_dir);
    snprintf(cand[2], MAX_PATH, "assets\\mouse.png");

    for (int i = 0; i < 3; i++) {
        DWORD attr = GetFileAttributesA(cand[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            char full[MAX_PATH];
            DWORD n = GetFullPathNameA(cand[i], MAX_PATH, full, NULL);
            if (n > 0 && n < MAX_PATH) {
                return dup_str_win(full);
            }
            return dup_str_win(cand[i]);
        }
    }
    return dup_str_win("mouse.png");
}
