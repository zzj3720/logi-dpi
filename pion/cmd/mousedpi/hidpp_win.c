#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <devpropdef.h>
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
    HANDLE  h;
    USHORT  in_len;
    USHORT  out_len;
    HANDLE  h_short;
    USHORT  short_in_len;
    USHORT  short_out_len;
    int     notify_only;
    uint8_t pend_buf[64];
    int     pend_len;
};

static int             g_win_inited = 0;
static char            g_exe_path[MAX_PATH] = {0};
static char            g_exe_dir[MAX_PATH]  = {0};
static char            g_cfg_path[MAX_PATH] = {0};

static volatile LONG64 g_last_mouse_tick = 0;
static SRWLOCK         g_raw_lock = SRWLOCK_INIT;
static int             g_win_watchers_started = 0;
static int             g_raw_stopping = 0;
static HPOWERNOTIFY    g_power_notify = NULL;
static HANDLE          g_raw_thread = NULL;
static HANDLE          g_raw_ready_ev = NULL;
static HWND            g_raw_hwnd = NULL;

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

int32_t hidpp_plat_wait_tray_ready(int32_t timeout_ms) {
    (void)timeout_ms;
    return 1;
}

void hidpp_plat_notify_tray_show(void) {
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
                /* Open with access=0 first so system-exclusive mouse/keyboard TLCs
                 * never trigger false ERROR_ACCESS_DENIED during enumeration. */
                HANDLE h = CreateFileA(
                    det->DevicePath,
                    0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    0,
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

/* DEVPKEY_Device_ContainerId = {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, pid = 2 */
static const DEVPROPKEY kDevPkeyContainerId = {
    { 0x8c7ed206u, 0x3f8au, 0x4827u, { 0xb3u, 0xabu, 0xaeu, 0x9eu, 0x1fu, 0xaeu, 0xfcu, 0x6cu } },
    2u
};

static int str_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static int guid_is_nonzero(const GUID *g) {
    const uint8_t *p = (const uint8_t *)g;
    for (size_t i = 0; i < sizeof(*g); i++) {
        if (p[i] != 0) return 1;
    }
    return 0;
}

static int get_dev_container_id(HDEVINFO info, SP_DEVINFO_DATA *devinfo, GUID *out_cid) {
    if (!devinfo || !out_cid) return 0;
    DEVPROPTYPE ptype = 0;
    DWORD req = 0;
    GUID cid;
    memset(&cid, 0, sizeof(cid));
    if (!SetupDiGetDevicePropertyW(
            info,
            devinfo,
            &kDevPkeyContainerId,
            &ptype,
            (PBYTE)&cid,
            (DWORD)sizeof(cid),
            &req,
            0)) {
        return 0;
    }
    if (ptype != DEVPROP_TYPE_GUID || req != sizeof(GUID) || !guid_is_nonzero(&cid)) {
        return 0;
    }
    *out_cid = cid;
    return 1;
}

static HANDLE open_short_tlc_for_device(const char *endpoint_id, DWORD desired_access,
                                        USHORT *out_in_len, USHORT *out_out_len) {
    if (!endpoint_id || !endpoint_id[0]) return INVALID_HANDLE_VALUE;

    uint16_t want_vid = 0, want_pid = 0;
    if (!parse_hex4_after(endpoint_id, "vid_", &want_vid) ||
        !parse_hex4_after(endpoint_id, "pid_", &want_pid)) {
        return INVALID_HANDLE_VALUE;
    }

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsA(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    /* Pass 1: resolve DEVPKEY_Device_ContainerId of the exact target endpoint_id. */
    GUID target_cid;
    memset(&target_cid, 0, sizeof(target_cid));
    int has_target_cid = 0;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(info, NULL, &hid_guid, idx, &did); idx++) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &did, NULL, 0, &req, NULL);
        if (req == 0) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, req);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        SP_DEVINFO_DATA devinfo;
        memset(&devinfo, 0, sizeof(devinfo));
        devinfo.cbSize = sizeof(devinfo);
        if (SetupDiGetDeviceInterfaceDetailA(info, &did, det, req, NULL, &devinfo)) {
            if (str_eq_ci(det->DevicePath, endpoint_id)) {
                has_target_cid = get_dev_container_id(info, &devinfo, &target_cid);
                HeapFree(GetProcessHeap(), 0, det);
                break;
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }

    /* Without a verified ContainerId, do not guess across potentially identical VID/PID receivers. */
    if (!has_target_cid) {
        SetupDiDestroyDeviceInfoList(info);
        return INVALID_HANDLE_VALUE;
    }

    /* Pass 2: find the 7-byte short HID++ TLC belonging to the SAME physical device
     * (strictly matching ContainerId GUID + vid_ + pid_). */
    HANDLE found = INVALID_HANDLE_VALUE;
    did.cbSize = sizeof(did);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(info, NULL, &hid_guid, idx, &did); idx++) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &did, NULL, 0, &req, NULL);
        if (req == 0) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, req);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        SP_DEVINFO_DATA devinfo;
        memset(&devinfo, 0, sizeof(devinfo));
        devinfo.cbSize = sizeof(devinfo);
        if (SetupDiGetDeviceInterfaceDetailA(info, &did, det, req, NULL, &devinfo)) {
            uint16_t vid = 0, pid = 0;
            int id_ok = parse_hex4_after(det->DevicePath, "vid_", &vid) && vid == want_vid &&
                        parse_hex4_after(det->DevicePath, "pid_", &pid) && pid == want_pid;
            int dev_match = 0;
            if (id_ok) {
                GUID cand_cid;
                if (get_dev_container_id(info, &devinfo, &cand_cid) &&
                    memcmp(&cand_cid, &target_cid, sizeof(GUID)) == 0) {
                    dev_match = 1;
                }
            }

            if (dev_match) {
                HANDLE probe = CreateFileA(
                    det->DevicePath,
                    0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    0,
                    NULL);
                if (probe != INVALID_HANDLE_VALUE) {
                    int is_short = 0;
                    USHORT in_l = 0, out_l = 0;
                    PHIDP_PREPARSED_DATA ppd = NULL;
                    if (HidD_GetPreparsedData(probe, &ppd)) {
                        HIDP_CAPS caps;
                        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS &&
                            caps.UsagePage == HIDPP_USAGE_PAGE_VENDOR &&
                            caps.InputReportByteLength == HIDPP_SHORT_LEN &&
                            (desired_access == GENERIC_READ || caps.OutputReportByteLength == HIDPP_SHORT_LEN)) {
                            is_short = 1;
                            in_l = caps.InputReportByteLength;
                            out_l = caps.OutputReportByteLength;
                        }
                        HidD_FreePreparsedData(ppd);
                    }
                    CloseHandle(probe);

                    if (is_short) {
                        HANDLE h = CreateFileA(
                            det->DevicePath,
                            desired_access,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED,
                            NULL);
                        if (h != INVALID_HANDLE_VALUE) {
                            if (out_in_len) *out_in_len = in_l;
                            if (out_out_len) *out_out_len = out_l;
                            HeapFree(GetProcessHeap(), 0, det);
                            found = h;
                            break;
                        }
                    }
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
    HANDLE h_short = INVALID_HANDLE_VALUE;
    USHORT in_len = HIDPP_LONG_LEN;
    USHORT out_len = HIDPP_LONG_LEN;
    USHORT short_in_len = HIDPP_SHORT_LEN;
    USHORT short_out_len = HIDPP_SHORT_LEN;

    if (notify_only) {
        /* Prefer the 7-byte short HID++ TLC on the SAME physical device (matched by ContainerId). */
        h = open_short_tlc_for_device(endpoint_id, GENERIC_READ, &in_len, &out_len);
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

    if (!notify_only && endpoint_id && endpoint_id[0]) {
        h_short = open_short_tlc_for_device(
            endpoint_id, GENERIC_READ | GENERIC_WRITE, &short_in_len, &short_out_len);
    }

    hidpp_dev_t *dev = (hidpp_dev_t *)calloc(1, sizeof(*dev));
    if (!dev) {
        CloseHandle(h);
        if (h_short != INVALID_HANDLE_VALUE) CloseHandle(h_short);
        return NULL;
    }
    dev->h = h;
    dev->in_len = in_len;
    dev->out_len = out_len;
    dev->h_short = h_short;
    dev->short_in_len = short_in_len;
    dev->short_out_len = short_out_len;
    dev->notify_only = notify_only ? 1 : 0;
    dev->pend_len = 0;
    return dev;
}

void hidpp_plat_close(hidpp_dev_t *dev) {
    if (!dev) return;
    if (dev->h != INVALID_HANDLE_VALUE) CloseHandle(dev->h);
    if (dev->h_short != INVALID_HANDLE_VALUE) CloseHandle(dev->h_short);
    free(dev);
}

void hidpp_plat_drain(hidpp_dev_t *dev) {
    if (!dev) return;
    dev->pend_len = 0;
    if (dev->h != INVALID_HANDLE_VALUE) (void)HidD_FlushQueue(dev->h);
    if (dev->h_short != INVALID_HANDLE_VALUE) (void)HidD_FlushQueue(dev->h_short);
}

int hidpp_plat_write(hidpp_dev_t *dev, const uint8_t *buf, size_t len) {
    if (!dev || dev->h == INVALID_HANDLE_VALUE || !buf || len == 0) return -1;

    HANDLE target = dev->h;
    USHORT target_out = dev->out_len;
    if (buf[0] == HIDPP_SHORT_REPORT_ID && len == HIDPP_SHORT_LEN &&
        dev->h_short != INVALID_HANDLE_VALUE) {
        target = dev->h_short;
        target_out = dev->short_out_len;
    }

    uint8_t pad[64];
    const uint8_t *wbuf = buf;
    size_t wlen = len;
    if (target_out > len && target_out <= sizeof(pad)) {
        memset(pad, 0, target_out);
        memcpy(pad, buf, len);
        wbuf = pad;
        wlen = target_out;
    }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    DWORD written = 0;
    BOOL ok = WriteFile(target, wbuf, (DWORD)wlen, &written, &ov);
    if (!ok) {
        if (GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 300);
            if (w == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(target, &ov, &written, FALSE);
            } else {
                /* Wait for final kernel completion and honor it if the write actually finished
                 * before CancelIoEx took effect (prevents replaying a landed write on another mouse). */
                CancelIoEx(target, &ov);
                ok = GetOverlappedResult(target, &ov, &written, TRUE);
            }
        }
    }
    CloseHandle(ov.hEvent);
    return ok ? (int)written : -1;
}

static int read_single_overlapped(HANDLE h, USHORT in_len, uint8_t *buf, size_t cap, int timeout_ms) {
    DWORD want = (DWORD)in_len;
    if (want == 0 || want > (DWORD)cap) want = (DWORD)cap;

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, want, &got, &ov);
    int rc = -1;
    if (ok) {
        rc = (int)got;
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
            if (w == WAIT_OBJECT_0) {
                if (GetOverlappedResult(h, &ov, &got, FALSE)) {
                    rc = (int)got;
                } else {
                    rc = -1;
                }
            } else if (w == WAIT_TIMEOUT) {
                /* Must wait for cancelled I/O to complete before freeing stack OVERLAPPED & event. */
                CancelIoEx(h, &ov);
                if (GetOverlappedResult(h, &ov, &got, TRUE) && got > 0) {
                    rc = (int)got;
                } else {
                    rc = 0;
                }
            } else {
                CancelIoEx(h, &ov);
                (void)GetOverlappedResult(h, &ov, &got, TRUE);
                rc = -1;
            }
        } else {
            rc = -1;
        }
    }
    CloseHandle(ov.hEvent);
    return rc;
}

static void stash_pending_frame(hidpp_dev_t *dev, const uint8_t *src, DWORD len) {
    if (!dev || !src || len == 0) return;
    size_t cpy = (size_t)len;
    if (cpy > sizeof(dev->pend_buf)) cpy = sizeof(dev->pend_buf);
    memcpy(dev->pend_buf, src, cpy);
    dev->pend_len = (int)cpy;
}

int hidpp_plat_read(hidpp_dev_t *dev, uint8_t *buf, size_t cap, int timeout_ms) {
    if (!dev || dev->h == INVALID_HANDLE_VALUE || !buf || cap == 0) return -1;

    if (dev->pend_len > 0) {
        size_t cpy = (size_t)dev->pend_len;
        if (cpy > cap) cpy = cap;
        memcpy(buf, dev->pend_buf, cpy);
        dev->pend_len = 0;
        return (int)cpy;
    }

    if (dev->h_short == INVALID_HANDLE_VALUE || dev->notify_only) {
        return read_single_overlapped(dev->h, dev->in_len, buf, cap, timeout_ms);
    }

    uint8_t b_long[64];
    uint8_t b_short[64];
    DWORD want_l = (DWORD)dev->in_len;
    if (want_l == 0 || want_l > sizeof(b_long)) want_l = sizeof(b_long);
    DWORD want_s = (DWORD)dev->short_in_len;
    if (want_s == 0 || want_s > sizeof(b_short)) want_s = sizeof(b_short);

    OVERLAPPED ov_l, ov_s;
    memset(&ov_l, 0, sizeof(ov_l));
    memset(&ov_s, 0, sizeof(ov_s));
    ov_l.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ov_s.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov_l.hEvent || !ov_s.hEvent) {
        if (ov_l.hEvent) CloseHandle(ov_l.hEvent);
        if (ov_s.hEvent) CloseHandle(ov_s.hEvent);
        return -1;
    }

    DWORD got_l = 0, got_s = 0;
    BOOL ok_l = ReadFile(dev->h, b_long, want_l, &got_l, &ov_l);
    int pend_l = (!ok_l && GetLastError() == ERROR_IO_PENDING) ? 1 : 0;

    if (ok_l) {
        size_t cpy = got_l > (DWORD)cap ? cap : (size_t)got_l;
        memcpy(buf, b_long, cpy);
        CloseHandle(ov_l.hEvent);
        CloseHandle(ov_s.hEvent);
        return (int)cpy;
    }

    BOOL ok_s = ReadFile(dev->h_short, b_short, want_s, &got_s, &ov_s);
    int pend_s = (!ok_s && GetLastError() == ERROR_IO_PENDING) ? 1 : 0;

    if (ok_s) {
        if (pend_l) {
            CancelIoEx(dev->h, &ov_l);
            if (GetOverlappedResult(dev->h, &ov_l, &got_l, TRUE) && got_l > 0) {
                stash_pending_frame(dev, b_long, got_l);
            }
        }
        size_t cpy = got_s > (DWORD)cap ? cap : (size_t)got_s;
        memcpy(buf, b_short, cpy);
        CloseHandle(ov_l.hEvent);
        CloseHandle(ov_s.hEvent);
        return (int)cpy;
    }

    int rc = -1;
    DWORD wait_ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;

    if (pend_l && pend_s) {
        HANDLE evs[2] = { ov_l.hEvent, ov_s.hEvent };
        DWORD w = WaitForMultipleObjects(2, evs, FALSE, wait_ms);
        if (w == WAIT_OBJECT_0) {
            CancelIoEx(dev->h_short, &ov_s);
            BOOL fin_s = GetOverlappedResult(dev->h_short, &ov_s, &got_s, TRUE);
            if (GetOverlappedResult(dev->h, &ov_l, &got_l, FALSE) && got_l > 0) {
                if (fin_s && got_s > 0) {
                    stash_pending_frame(dev, b_short, got_s);
                }
                size_t cpy = got_l > (DWORD)cap ? cap : (size_t)got_l;
                memcpy(buf, b_long, cpy);
                rc = (int)cpy;
            } else if (fin_s && got_s > 0) {
                size_t cpy = got_s > (DWORD)cap ? cap : (size_t)got_s;
                memcpy(buf, b_short, cpy);
                rc = (int)cpy;
            } else {
                rc = -1;
            }
        } else if (w == WAIT_OBJECT_0 + 1) {
            CancelIoEx(dev->h, &ov_l);
            BOOL fin_l = GetOverlappedResult(dev->h, &ov_l, &got_l, TRUE);
            if (GetOverlappedResult(dev->h_short, &ov_s, &got_s, FALSE) && got_s > 0) {
                if (fin_l && got_l > 0) {
                    stash_pending_frame(dev, b_long, got_l);
                }
                size_t cpy = got_s > (DWORD)cap ? cap : (size_t)got_s;
                memcpy(buf, b_short, cpy);
                rc = (int)cpy;
            } else if (fin_l && got_l > 0) {
                size_t cpy = got_l > (DWORD)cap ? cap : (size_t)got_l;
                memcpy(buf, b_long, cpy);
                rc = (int)cpy;
            } else {
                rc = -1;
            }
        } else if (w == WAIT_TIMEOUT) {
            CancelIoEx(dev->h, &ov_l);
            CancelIoEx(dev->h_short, &ov_s);
            BOOL fin_l = GetOverlappedResult(dev->h, &ov_l, &got_l, TRUE);
            BOOL fin_s = GetOverlappedResult(dev->h_short, &ov_s, &got_s, TRUE);
            if (fin_l && got_l > 0) {
                if (fin_s && got_s > 0) {
                    stash_pending_frame(dev, b_short, got_s);
                }
                size_t cpy = got_l > (DWORD)cap ? cap : (size_t)got_l;
                memcpy(buf, b_long, cpy);
                rc = (int)cpy;
            } else if (fin_s && got_s > 0) {
                size_t cpy = got_s > (DWORD)cap ? cap : (size_t)got_s;
                memcpy(buf, b_short, cpy);
                rc = (int)cpy;
            } else {
                rc = 0;
            }
        } else {
            CancelIoEx(dev->h, &ov_l);
            CancelIoEx(dev->h_short, &ov_s);
            (void)GetOverlappedResult(dev->h, &ov_l, &got_l, TRUE);
            (void)GetOverlappedResult(dev->h_short, &ov_s, &got_s, TRUE);
            rc = -1;
        }
    } else if (pend_l) {
        DWORD w = WaitForSingleObject(ov_l.hEvent, wait_ms);
        if (w == WAIT_OBJECT_0) {
            if (GetOverlappedResult(dev->h, &ov_l, &got_l, FALSE)) {
                size_t cpy = got_l > (DWORD)cap ? cap : (size_t)got_l;
                memcpy(buf, b_long, cpy);
                rc = (int)cpy;
            }
        } else if (w == WAIT_TIMEOUT) {
            CancelIoEx(dev->h, &ov_l);
            if (GetOverlappedResult(dev->h, &ov_l, &got_l, TRUE) && got_l > 0) {
                size_t cpy = got_l > (DWORD)cap ? cap : (size_t)got_l;
                memcpy(buf, b_long, cpy);
                rc = (int)cpy;
            } else {
                rc = 0;
            }
        } else {
            CancelIoEx(dev->h, &ov_l);
            (void)GetOverlappedResult(dev->h, &ov_l, &got_l, TRUE);
        }
    } else if (pend_s) {
        DWORD w = WaitForSingleObject(ov_s.hEvent, wait_ms);
        if (w == WAIT_OBJECT_0) {
            if (GetOverlappedResult(dev->h_short, &ov_s, &got_s, FALSE)) {
                size_t cpy = got_s > (DWORD)cap ? cap : (size_t)got_s;
                memcpy(buf, b_short, cpy);
                rc = (int)cpy;
            }
        } else if (w == WAIT_TIMEOUT) {
            CancelIoEx(dev->h_short, &ov_s);
            if (GetOverlappedResult(dev->h_short, &ov_s, &got_s, TRUE) && got_s > 0) {
                size_t cpy = got_s > (DWORD)cap ? cap : (size_t)got_s;
                memcpy(buf, b_short, cpy);
                rc = (int)cpy;
            } else {
                rc = 0;
            }
        } else {
            CancelIoEx(dev->h_short, &ov_s);
            (void)GetOverlappedResult(dev->h_short, &ov_s, &got_s, TRUE);
        }
    }

    CloseHandle(ov_l.hEvent);
    CloseHandle(ov_s.hEvent);
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
    } else if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    } else if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI raw_input_thread(LPVOID param) {
    HANDLE ready_ev = (HANDLE)param;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = raw_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MouseDPIRawInput";
    RegisterClassW(&wc);

    HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!h) {
        if (ready_ev) SetEvent(ready_ev);
        return 0;
    }

    AcquireSRWLockExclusive(&g_raw_lock);
    int stopping = g_raw_stopping;
    if (!stopping) {
        g_raw_hwnd = h;
    }
    ReleaseSRWLockExclusive(&g_raw_lock);

    if (stopping) {
        DestroyWindow(h);
        if (ready_ev) SetEvent(ready_ev);
        return 0;
    }

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02; /* Generic Desktop Mouse */
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = h;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    if (ready_ev) SetEvent(ready_ev);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        DispatchMessageW(&m);
    }
    if (IsWindow(h)) {
        DestroyWindow(h);
    }

    AcquireSRWLockExclusive(&g_raw_lock);
    if (g_raw_hwnd == h) {
        g_raw_hwnd = NULL;
    }
    ReleaseSRWLockExclusive(&g_raw_lock);
    return 0;
}

void hidpp_plat_start_watchers(void) {
    AcquireSRWLockExclusive(&g_raw_lock);
    if (g_win_watchers_started) {
        ReleaseSRWLockExclusive(&g_raw_lock);
        return;
    }
    g_win_watchers_started = 1;
    g_raw_stopping = 0;
    InterlockedExchange64(&g_last_mouse_tick, (LONG64)GetTickCount64());

    static DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS power = { win_power_cb, NULL };
    PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &power, &g_power_notify);
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_raw_ready_ev = ev;
    HANDLE th = CreateThread(NULL, 0, raw_input_thread, ev, 0, NULL);
    g_raw_thread = th;
    ReleaseSRWLockExclusive(&g_raw_lock);

    if (th && ev) {
        WaitForSingleObject(ev, 500);
    }
}

void hidpp_plat_stop_watchers(void) {
    AcquireSRWLockExclusive(&g_raw_lock);
    if (!g_win_watchers_started) {
        ReleaseSRWLockExclusive(&g_raw_lock);
        return;
    }
    g_win_watchers_started = 0;
    g_raw_stopping = 1;
    HPOWERNOTIFY pwr = g_power_notify;
    g_power_notify = NULL;
    HWND h = g_raw_hwnd;
    HANDLE th = g_raw_thread;
    g_raw_thread = NULL;
    HANDLE ev = g_raw_ready_ev;
    ReleaseSRWLockExclusive(&g_raw_lock);

    if (pwr) {
        PowerUnregisterSuspendResumeNotification(pwr);
    }
    if (h) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    int thread_exited = 1;
    if (th) {
        DWORD wr = WaitForSingleObject(th, 2000);
        if (wr != WAIT_OBJECT_0) {
            thread_exited = 0;
        }
        CloseHandle(th);
    }
    if (thread_exited && ev) {
        AcquireSRWLockExclusive(&g_raw_lock);
        if (g_raw_ready_ev == ev) {
            g_raw_ready_ev = NULL;
        }
        ReleaseSRWLockExclusive(&g_raw_lock);
        CloseHandle(ev);
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
