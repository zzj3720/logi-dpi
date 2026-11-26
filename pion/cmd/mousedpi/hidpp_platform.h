#ifndef HIDPP_PLATFORM_H
#define HIDPP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- HID++ 2.0 protocol constants ---- */
#define HIDPP_VID_LOGITECH        0x046Du
#define HIDPP_USAGE_PAGE_VENDOR   0xFF00u

#define HIDPP_SHORT_REPORT_ID     0x10u
#define HIDPP_LONG_REPORT_ID      0x11u
#define HIDPP_SHORT_LEN           7
#define HIDPP_LONG_LEN            20

#define HIDPP_SWID                0x0Au
#define HIDPP_XFER_TIMEOUT_MS     350

#define HIDPP_FEAT_IROOT          0x0000u
#define HIDPP_FEAT_DEV_NAME       0x0005u
#define HIDPP_FEAT_BATTERY        0x1004u
#define HIDPP_FEAT_EXT_DPI        0x2202u
#define HIDPP_FEAT_ONBOARD        0x8100u

#define HIDPP_DEFAULT_DEVICE_NAME "PRO X2 SUPERSTRIKE"
#define HIDPP_DEFAULT_DPI         800
#define HIDPP_DEFAULT_LOD         2
#define HIDPP_MIN_DPI             100
#define HIDPP_MAX_DPI             44000

#define HIDPP_MAX_ENDPOINTS       32
#define HIDPP_ENDPOINT_ID_MAX     256

/* ---- Endpoint classification & priority ---- */
typedef enum hidpp_endpoint_kind {
    HIDPP_KIND_LIGHTSPEED_RECEIVER = 1, /* dev_idx = 0x01 */
    HIDPP_KIND_DIRECT_WIRED        = 2, /* dev_idx = 0xFF */
    HIDPP_KIND_MULTI_RECEIVER      = 3  /* dev_idx = 1..6 */
} hidpp_endpoint_kind_t;

typedef struct hidpp_endpoint_info {
    char                  id[HIDPP_ENDPOINT_ID_MAX];
    uint16_t              vid;
    uint16_t              pid;
    hidpp_endpoint_kind_t kind;
} hidpp_endpoint_info_t;

static inline hidpp_endpoint_kind_t hidpp_classify_pid(uint16_t pid) {
    switch (pid) {
        case 0xC54Du:
        case 0xC547u:
        case 0xC539u:
        case 0xC545u:
        case 0xC53Fu:
        case 0xC541u:
            return HIDPP_KIND_LIGHTSPEED_RECEIVER;
        case 0xC548u:
        case 0xC52Bu:
        case 0xC532u:
        case 0xC534u:
            return HIDPP_KIND_MULTI_RECEIVER;
        default:
            if ((pid & 0xFF00u) == 0xC500u) {
                return HIDPP_KIND_MULTI_RECEIVER;
            }
            return HIDPP_KIND_DIRECT_WIRED;
    }
}

/* ---- HID Report Descriptor capabilities (parsed by hidpp_core.c) ---- */
typedef struct hidpp_rdesc_caps {
    int      has_ff00;
    uint32_t in_bytes_10;
    uint32_t out_bytes_10;
    uint32_t in_bytes_11;
    uint32_t out_bytes_11;
} hidpp_rdesc_caps_t;

/* Pure C USB HID 1.11 Report Descriptor parser (supports Global Push/Pop stack
 * and per-Report-ID Input/Output bit accumulation). */
void hidpp_parse_report_descriptor(const uint8_t *desc, size_t len, hidpp_rdesc_caps_t *out);

/* ---- Shared POSIX atomic config.ini helpers (implemented in hidpp_core.c) ---- */
int hidpp_posix_config_read_file(const char *path, int32_t *out_dpi, int32_t *out_lod);
int hidpp_posix_config_write_file(const char *path, int32_t dpi, int32_t lod);

/* ---- Opaque platform HID device handle ---- */
typedef struct hidpp_dev hidpp_dev_t;

/* ---- Core hooks callable from platform layer ---- */
void hidpp_core_signal_wake(void);
void hidpp_core_signal_wake_reconnect(void);
void hidpp_core_signal_topology_change(void);
int  hidpp_core_is_stopping(void);

/* ---- Platform transport & OS hooks (implemented by win / macos / linux / test mock) ---- */
void     hidpp_plat_init(void);
void     hidpp_plat_start_watchers(void);
void     hidpp_plat_stop_watchers(void);
int      hidpp_plat_needs_notify_thread(void);

/* Enumerates candidate VID 0x046D HID++ endpoints, sorted by kind ascending.
 * Sets *out_perm_denied = 1 if any Logitech HID++ candidate was denied by permissions. */
int      hidpp_plat_enumerate(hidpp_endpoint_info_t *out, int max_count, int *out_perm_denied);

/* Opens a candidate endpoint. notify_only != 0 opens for reading 0x41 notifications. */
hidpp_dev_t *hidpp_plat_open(const char *endpoint_id, int notify_only, int *out_perm_denied);
void     hidpp_plat_close(hidpp_dev_t *dev);
void     hidpp_plat_drain(hidpp_dev_t *dev);
int      hidpp_plat_write(hidpp_dev_t *dev, const uint8_t *buf, size_t len);
/* Returns >0 bytes read, 0 on timeout, <0 on fatal error / disconnect. */
int      hidpp_plat_read(hidpp_dev_t *dev, uint8_t *buf, size_t cap, int timeout_ms);

uint64_t hidpp_plat_now_ms(void);
void     hidpp_plat_sleep_ms(uint32_t ms);
/* Returns ms since last mouse activity, or UINT64_MAX if unsupported (disables 2s gentle poll). */
uint64_t hidpp_plat_mouse_idle_ms(void);

int32_t  hidpp_plat_started_hidden(void);
int32_t  hidpp_plat_can_hide_to_tray(void);
int32_t  hidpp_plat_wait_tray_ready(int32_t timeout_ms);
int32_t  hidpp_plat_get_autostart(void);
int32_t  hidpp_plat_set_autostart(int32_t enable);
char    *hidpp_plat_picture_path_dup(void);

int      hidpp_plat_config_read(int32_t *out_dpi, int32_t *out_lod);
int      hidpp_plat_config_write(int32_t dpi, int32_t lod);

/* ---- Public C ABI (28 exported symbols) ---- */
int32_t mousedpi_started_hidden(void);
int32_t mousedpi_peek_config(int32_t which);
int32_t mousedpi_init_and_restore(void);
void    mousedpi_request_refresh(void);
void    mousedpi_request_set(int32_t dpi, int32_t lod);
void    mousedpi_request_set_seq(int32_t seq, int32_t dpi, int32_t lod);
int32_t mousedpi_state_rev(void);
int32_t mousedpi_wait_rev(int32_t seen);
void    mousedpi_stop_waits(void);
int32_t mousedpi_get_connected(void);
int32_t mousedpi_get_dpi_x(void);
int32_t mousedpi_get_lod(void);
int32_t mousedpi_get_battery(void);
int32_t mousedpi_get_autostart(void);
int32_t mousedpi_set_autostart(int32_t enable);

void    mousedpi_request_wake_restore(void);
void    mousedpi_snapshot_capture(void);
int32_t mousedpi_snap_rev(void);
int32_t mousedpi_snap_applied_seq(void);
int32_t mousedpi_snap_connected(void);
int32_t mousedpi_snap_dpi_x(void);
int32_t mousedpi_snap_lod(void);
int32_t mousedpi_snap_battery(void);
int32_t mousedpi_snap_perm_denied(void);
char   *mousedpi_snap_device_name_dup(void);
char   *mousedpi_picture_path_dup(void);
int32_t mousedpi_can_hide_to_tray(void);
int32_t mousedpi_wait_tray_ready(int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* HIDPP_PLATFORM_H */
