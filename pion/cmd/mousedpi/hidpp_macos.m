#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <CoreGraphics/CoreGraphics.h>
#import <mach-o/dyld.h>
#import <crt_externs.h>

#include "hidpp_platform.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MACOS_RING_CAP 32

typedef struct macos_ring_item {
    uint8_t  data[64];
    uint16_t len;
} macos_ring_item_t;

typedef struct macos_slot {
    char                  id[HIDPP_ENDPOINT_ID_MAX];
    uint16_t              vid;
    uint16_t              pid;
    hidpp_endpoint_kind_t kind;

    IOHIDDeviceRef        device;
    uint8_t              *cb_buf;
    size_t                cb_buf_len;

    pthread_mutex_t       lock;
    pthread_cond_t        cv;
    macos_ring_item_t     ring[MACOS_RING_CAP];
    int                   head;
    int                   tail;
    int                   count;
    int                   alive;
    int                   in_flight;

    struct macos_slot    *next;
} macos_slot_t;

struct hidpp_dev {
    macos_slot_t *slot;
};

#define MACOS_DENIED_CAP 32

static pthread_mutex_t g_slots_lock = PTHREAD_MUTEX_INITIALIZER;
static macos_slot_t   *g_slots_head = NULL;
static uint64_t        g_next_slot_id = 1;
static IOHIDDeviceRef  g_denied_devs[MACOS_DENIED_CAP];
static int             g_denied_count = 0;
static int             g_mgr_open_denied = 0;

static pthread_mutex_t g_mgr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_mgr_ready_cv = PTHREAD_COND_INITIALIZER;
static int             g_mgr_started = 0;
static int             g_mgr_ready = 0;
static int             g_mgr_stopping = 0;
static int             g_mgr_stopped = 0;
static pthread_t       g_mgr_thread;
static CFRunLoopRef    g_mgr_runloop = NULL;

static void denied_add_locked(IOHIDDeviceRef device) {
    if (!device) return;
    for (int i = 0; i < g_denied_count; i++) {
        if (g_denied_devs[i] == device) return;
    }
    if (g_denied_count < MACOS_DENIED_CAP) {
        g_denied_devs[g_denied_count++] = device;
    }
}

static void denied_remove_locked(IOHIDDeviceRef device) {
    if (!device) return;
    for (int i = 0; i < g_denied_count; i++) {
        if (g_denied_devs[i] == device) {
            g_denied_devs[i] = g_denied_devs[--g_denied_count];
            g_denied_devs[g_denied_count] = NULL;
            return;
        }
    }
}

static int             g_paths_inited = 0;
static char            g_exe_path[PATH_MAX] = {0};
static char            g_exe_dir[PATH_MAX]  = {0};
static char            g_cfg_path[PATH_MAX] = {0};

static void init_paths(void) {
    if (g_paths_inited) return;
    g_paths_inited = 1;

    uint32_t sz = sizeof(g_exe_path);
    char raw[PATH_MAX];
    if (_NSGetExecutablePath(raw, &sz) == 0) {
        if (!realpath(raw, g_exe_path)) {
            snprintf(g_exe_path, sizeof(g_exe_path), "%s", raw);
        }
        snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", g_exe_path);
        char *slash = strrchr(g_exe_dir, '/');
        if (slash) *slash = '\0';
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(g_cfg_path, sizeof(g_cfg_path),
                 "%s/Library/Application Support/MouseDPI/config.ini", home);
    }
}

static uint32_t cf_prop_u32(IOHIDDeviceRef dev, CFStringRef key) {
    CFTypeRef ref = IOHIDDeviceGetProperty(dev, key);
    if (!ref || CFGetTypeID(ref) != CFNumberGetTypeID()) return 0;
    int64_t v = 0;
    if (CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, &v) && v >= 0) {
        return (uint32_t)v;
    }
    return 0;
}

static int device_has_ff00_usage_page(IOHIDDeviceRef dev) {
    if (cf_prop_u32(dev, CFSTR(kIOHIDPrimaryUsagePageKey)) == HIDPP_USAGE_PAGE_VENDOR) {
        return 1;
    }
    CFTypeRef pairs_ref = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDDeviceUsagePairsKey));
    if (pairs_ref && CFGetTypeID(pairs_ref) == CFArrayGetTypeID()) {
        CFArrayRef arr = (CFArrayRef)pairs_ref;
        CFIndex n = CFArrayGetCount(arr);
        for (CFIndex i = 0; i < n; i++) {
            CFTypeRef item = CFArrayGetValueAtIndex(arr, i);
            if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID()) continue;
            CFDictionaryRef dict = (CFDictionaryRef)item;
            CFTypeRef up = CFDictionaryGetValue(dict, CFSTR(kIOHIDDeviceUsagePageKey));
            if (up && CFGetTypeID(up) == CFNumberGetTypeID()) {
                int64_t v = 0;
                if (CFNumberGetValue((CFNumberRef)up, kCFNumberSInt64Type, &v) &&
                    (uint32_t)v == HIDPP_USAGE_PAGE_VENDOR) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void input_report_cb(void *ctx, IOReturn res, void *sender,
                            IOHIDReportType type, uint32_t reportID,
                            uint8_t *report, CFIndex reportLength) {
    (void)sender;
    (void)type;
    if (res != kIOReturnSuccess || !ctx || !report || reportLength <= 0) return;

    macos_slot_t *slot = (macos_slot_t *)ctx;
    uint8_t norm[64];
    size_t norm_len = 0;

    if (reportID != 0 && report[0] != (uint8_t)reportID) {
        norm[0] = (uint8_t)reportID;
        size_t cpy = (size_t)reportLength;
        if (cpy > sizeof(norm) - 1) cpy = sizeof(norm) - 1;
        memcpy(norm + 1, report, cpy);
        norm_len = cpy + 1;
    } else {
        size_t cpy = (size_t)reportLength;
        if (cpy > sizeof(norm)) cpy = sizeof(norm);
        memcpy(norm, report, cpy);
        norm_len = cpy;
    }

    /* Direct 0x41 wireless connection/wake notification dispatch (no separate notify thread). */
    if (norm_len >= HIDPP_SHORT_LEN &&
        (norm[0] == HIDPP_SHORT_REPORT_ID || norm[0] == HIDPP_LONG_REPORT_ID) &&
        norm[2] == 0x41u &&
        ((norm[4] & 0x40u) == 0u)) {
        hidpp_core_signal_wake();
    }

    pthread_mutex_lock(&slot->lock);
    if (slot->alive) {
        if (slot->count == MACOS_RING_CAP) {
            /* Drop oldest frame if consumer is behind */
            slot->head = (slot->head + 1) % MACOS_RING_CAP;
            slot->count--;
        }
        macos_ring_item_t *it = &slot->ring[slot->tail];
        memcpy(it->data, norm, norm_len);
        it->len = (uint16_t)norm_len;
        slot->tail = (slot->tail + 1) % MACOS_RING_CAP;
        slot->count++;
        pthread_cond_broadcast(&slot->cv);
    }
    pthread_mutex_unlock(&slot->lock);
}

static void device_matched_cb(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef device) {
    (void)ctx;
    (void)sender;
    if (res != kIOReturnSuccess || !device) return;
    if (!device_has_ff00_usage_page(device)) return;

    uint16_t vid = (uint16_t)cf_prop_u32(device, CFSTR(kIOHIDVendorIDKey));
    uint16_t pid = (uint16_t)cf_prop_u32(device, CFSTR(kIOHIDProductIDKey));
    if (vid != HIDPP_VID_LOGITECH) return;

    IOReturn open_rc = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (open_rc != kIOReturnSuccess) {
        if (open_rc == kIOReturnNotPermitted) {
            pthread_mutex_lock(&g_slots_lock);
            denied_add_locked(device);
            pthread_mutex_unlock(&g_slots_lock);
        }
        return;
    }

    uint32_t max_in = cf_prop_u32(device, CFSTR(kIOHIDMaxInputReportSizeKey));
    if (max_in < 64u) max_in = 64u;

    macos_slot_t *slot = (macos_slot_t *)calloc(1, sizeof(*slot));
    if (!slot) {
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        return;
    }
    slot->cb_buf = (uint8_t *)calloc(1, max_in);
    if (!slot->cb_buf) {
        free(slot);
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        return;
    }
    slot->cb_buf_len = max_in;
    slot->device = (IOHIDDeviceRef)CFRetain(device);
    slot->vid = vid;
    slot->pid = pid;
    slot->kind = hidpp_classify_pid(pid);
    slot->alive = 1;
    slot->in_flight = 0;
    pthread_mutex_init(&slot->lock, NULL);
    pthread_cond_init(&slot->cv, NULL);

    pthread_mutex_lock(&g_slots_lock);
    denied_remove_locked(device);
    g_mgr_open_denied = 0;
    uint64_t sid = g_next_slot_id++;
    snprintf(slot->id, sizeof(slot->id), "iokit:%04x:%04x:%llu",
             (unsigned)vid, (unsigned)pid, (unsigned long long)sid);
    slot->next = g_slots_head;
    g_slots_head = slot;
    pthread_mutex_unlock(&g_slots_lock);

    IOHIDDeviceRegisterInputReportCallback(
        device, slot->cb_buf, (CFIndex)slot->cb_buf_len, input_report_cb, slot);

    /* Hotplug after initial manager startup: trigger topology change & restore/refresh. */
    pthread_mutex_lock(&g_mgr_lock);
    int ready = g_mgr_ready;
    pthread_mutex_unlock(&g_mgr_lock);
    if (ready) {
        hidpp_core_signal_topology_change();
    }
}

static void destroy_slot(macos_slot_t *slot) {
    if (!slot) return;
    IOHIDDeviceRegisterInputReportCallback(
        slot->device, slot->cb_buf, (CFIndex)slot->cb_buf_len, NULL, slot);

    pthread_mutex_lock(&slot->lock);
    slot->alive = 0;
    pthread_cond_broadcast(&slot->cv);
    while (slot->in_flight > 0) {
        pthread_cond_wait(&slot->cv, &slot->lock);
    }
    pthread_mutex_unlock(&slot->lock);

    IOHIDDeviceClose(slot->device, kIOHIDOptionsTypeNone);
    CFRelease(slot->device);
    pthread_cond_destroy(&slot->cv);
    pthread_mutex_destroy(&slot->lock);
    free(slot->cb_buf);
    free(slot);
}

static void device_removed_cb(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef device) {
    (void)ctx;
    (void)res;
    (void)sender;
    if (!device) return;

    macos_slot_t *removed = NULL;
    pthread_mutex_lock(&g_slots_lock);
    denied_remove_locked(device);
    macos_slot_t **pp = &g_slots_head;
    while (*pp) {
        if ((*pp)->device == device) {
            removed = *pp;
            *pp = removed->next;
            removed->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_slots_lock);

    if (removed) {
        destroy_slot(removed);
        hidpp_core_signal_topology_change();
    }
}

static void *hid_mgr_thread_main(void *unused) {
    (void)unused;
    @autoreleasepool {
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (mgr) {
            NSDictionary *match = @{
                @(kIOHIDVendorIDKey): @(HIDPP_VID_LOGITECH),
                @(kIOHIDDeviceUsagePageKey): @(HIDPP_USAGE_PAGE_VENDOR),
            };
            IOHIDManagerSetDeviceMatching(mgr, (__bridge CFDictionaryRef)match);
            IOHIDManagerRegisterDeviceMatchingCallback(mgr, device_matched_cb, NULL);
            IOHIDManagerRegisterDeviceRemovalCallback(mgr, device_removed_cb, NULL);

            CFRunLoopRef rl = CFRunLoopGetCurrent();
            IOHIDManagerScheduleWithRunLoop(mgr, rl, kCFRunLoopDefaultMode);

            IOReturn rc = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
            if (rc == kIOReturnNotPermitted) {
                pthread_mutex_lock(&g_slots_lock);
                g_mgr_open_denied = 1;
                pthread_mutex_unlock(&g_slots_lock);
            }

            /* Process initial device enumeration callbacks before signaling ready. */
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);

            pthread_mutex_lock(&g_mgr_lock);
            g_mgr_runloop = (CFRunLoopRef)CFRetain(rl);
            g_mgr_ready = 1;
            int should_run = !g_mgr_stopping;
            pthread_cond_broadcast(&g_mgr_ready_cv);
            pthread_mutex_unlock(&g_mgr_lock);

            if (should_run) {
                CFRunLoopRun();
            }

            IOHIDManagerUnscheduleFromRunLoop(mgr, rl, kCFRunLoopDefaultMode);
            IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
            CFRelease(mgr);

            /* Drain any remaining slots on shutdown */
            for (;;) {
                pthread_mutex_lock(&g_slots_lock);
                macos_slot_t *s = g_slots_head;
                if (s) {
                    g_slots_head = s->next;
                    s->next = NULL;
                }
                pthread_mutex_unlock(&g_slots_lock);
                if (!s) break;
                destroy_slot(s);
            }

            pthread_mutex_lock(&g_slots_lock);
            g_denied_count = 0;
            g_mgr_open_denied = 0;
            pthread_mutex_unlock(&g_slots_lock);

            pthread_mutex_lock(&g_mgr_lock);
            if (g_mgr_runloop) {
                CFRelease(g_mgr_runloop);
                g_mgr_runloop = NULL;
            }
            pthread_mutex_unlock(&g_mgr_lock);
        } else {
            pthread_mutex_lock(&g_mgr_lock);
            g_mgr_ready = 1;
            pthread_cond_broadcast(&g_mgr_ready_cv);
            pthread_mutex_unlock(&g_mgr_lock);
        }
    }
    return NULL;
}

static void ensure_hid_mgr_started(void) {
    pthread_mutex_lock(&g_mgr_lock);
    if (g_mgr_stopped || g_mgr_stopping || hidpp_core_is_stopping()) {
        pthread_mutex_unlock(&g_mgr_lock);
        return;
    }
    if (!g_mgr_started) {
        g_mgr_started = 1;
        g_mgr_ready = 0;
        if (pthread_create(&g_mgr_thread, NULL, hid_mgr_thread_main, NULL) == 0) {
            while (!g_mgr_ready) {
                pthread_cond_wait(&g_mgr_ready_cv, &g_mgr_lock);
            }
        } else {
            g_mgr_started = 0;
        }
    }
    pthread_mutex_unlock(&g_mgr_lock);
}

void hidpp_plat_init(void) {
    init_paths();
    ensure_hid_mgr_started();
}

void hidpp_plat_start_watchers(void) {
    ensure_hid_mgr_started();
}

void hidpp_plat_stop_watchers(void) {
    pthread_mutex_lock(&g_mgr_lock);
    g_mgr_stopping = 1;
    g_mgr_stopped = 1;
    int was_started = g_mgr_started;
    if (was_started) {
        while (!g_mgr_ready) {
            pthread_cond_wait(&g_mgr_ready_cv, &g_mgr_lock);
        }
    }
    CFRunLoopRef rl = g_mgr_runloop ? (CFRunLoopRef)CFRetain(g_mgr_runloop) : NULL;
    pthread_mutex_unlock(&g_mgr_lock);

    if (rl) {
        CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
            CFRunLoopStop(rl);
        });
        CFRunLoopWakeUp(rl);
        CFRelease(rl);
    }
    if (was_started) {
        pthread_join(g_mgr_thread, NULL);
        pthread_mutex_lock(&g_mgr_lock);
        g_mgr_started = 0;
        g_mgr_ready = 0;
        pthread_mutex_unlock(&g_mgr_lock);
    }
}

int hidpp_plat_needs_notify_thread(void) {
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
    ensure_hid_mgr_started();
    int count = 0;
    int alive_total = 0;

    pthread_mutex_lock(&g_slots_lock);
    for (macos_slot_t *s = g_slots_head; s; s = s->next) {
        pthread_mutex_lock(&s->lock);
        int alive = s->alive;
        pthread_mutex_unlock(&s->lock);
        if (!alive) continue;
        alive_total++;

        if (count < max_count) {
            hidpp_endpoint_info_t *ep = &out[count++];
            memset(ep, 0, sizeof(*ep));
            snprintf(ep->id, sizeof(ep->id), "%s", s->id);
            ep->vid = s->vid;
            ep->pid = s->pid;
            ep->kind = s->kind;
        }
    }
    if (out_perm_denied) {
        *out_perm_denied = (alive_total == 0 && (g_denied_count > 0 || g_mgr_open_denied)) ? 1 : 0;
    }
    pthread_mutex_unlock(&g_slots_lock);

    if (count > 1) {
        qsort(out, (size_t)count, sizeof(hidpp_endpoint_info_t), cmp_endpoint_kind);
    }
    return count;
}

hidpp_dev_t *hidpp_plat_open(const char *endpoint_id, int notify_only, int *out_perm_denied) {
    (void)notify_only;
    ensure_hid_mgr_started();
    if (out_perm_denied) *out_perm_denied = 0;
    if (!endpoint_id || !endpoint_id[0]) return NULL;

    macos_slot_t *found = NULL;
    int alive_total = 0;
    pthread_mutex_lock(&g_slots_lock);
    for (macos_slot_t *s = g_slots_head; s; s = s->next) {
        pthread_mutex_lock(&s->lock);
        int alive = s->alive;
        if (alive) {
            alive_total++;
            if (!found && strcmp(s->id, endpoint_id) == 0) {
                s->in_flight++;
                found = s;
            }
        }
        pthread_mutex_unlock(&s->lock);
    }
    if (!found && out_perm_denied) {
        *out_perm_denied = (alive_total == 0 && (g_denied_count > 0 || g_mgr_open_denied)) ? 1 : 0;
    }
    pthread_mutex_unlock(&g_slots_lock);

    if (!found) return NULL;
    hidpp_dev_t *dev = (hidpp_dev_t *)malloc(sizeof(*dev));
    if (!dev) {
        pthread_mutex_lock(&found->lock);
        found->in_flight--;
        if (found->in_flight == 0 && !found->alive) pthread_cond_broadcast(&found->cv);
        pthread_mutex_unlock(&found->lock);
        return NULL;
    }
    dev->slot = found;
    return dev;
}

void hidpp_plat_close(hidpp_dev_t *dev) {
    if (!dev) return;
    macos_slot_t *slot = dev->slot;
    if (slot) {
        pthread_mutex_lock(&slot->lock);
        slot->in_flight--;
        if (slot->in_flight == 0 && !slot->alive) {
            pthread_cond_broadcast(&slot->cv);
        }
        pthread_mutex_unlock(&slot->lock);
    }
    free(dev);
}

void hidpp_plat_drain(hidpp_dev_t *dev) {
    if (!dev || !dev->slot) return;
    macos_slot_t *slot = dev->slot;
    pthread_mutex_lock(&slot->lock);
    slot->head = 0;
    slot->tail = 0;
    slot->count = 0;
    pthread_mutex_unlock(&slot->lock);
}

int hidpp_plat_write(hidpp_dev_t *dev, const uint8_t *buf, size_t len) {
    if (!dev || !dev->slot || !buf || len == 0) return -1;
    macos_slot_t *slot = dev->slot;

    pthread_mutex_lock(&slot->lock);
    int alive = slot->alive;
    IOHIDDeviceRef device = slot->device;
    pthread_mutex_unlock(&slot->lock);
    if (!alive) return -1;

    /* Call IOHIDDeviceSetReport without holding slot->lock, passing full buffer including report ID. */
    IOReturn rc = IOHIDDeviceSetReport(
        device, kIOHIDReportTypeOutput, (CFIndex)buf[0], buf, (CFIndex)len);
    if (rc == kIOReturnNotPermitted) {
        pthread_mutex_lock(&g_slots_lock);
        denied_add_locked(device);
        pthread_mutex_unlock(&g_slots_lock);
        return -1;
    }
    return (rc == kIOReturnSuccess) ? (int)len : -1;
}

int hidpp_plat_read(hidpp_dev_t *dev, uint8_t *buf, size_t cap, int timeout_ms) {
    if (!dev || !dev->slot || !buf || cap == 0) return -1;
    macos_slot_t *slot = dev->slot;

    pthread_mutex_lock(&slot->lock);
    while (slot->count == 0 && slot->alive && !hidpp_core_is_stopping()) {
        if (timeout_ms <= 0) {
            pthread_mutex_unlock(&slot->lock);
            return 0;
        }
        struct timespec rel;
        rel.tv_sec = (time_t)(timeout_ms / 1000);
        rel.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
        int rc = pthread_cond_timedwait_relative_np(&slot->cv, &slot->lock, &rel);
        if (rc == ETIMEDOUT) break;
    }

    if (!slot->alive) {
        pthread_mutex_unlock(&slot->lock);
        return -1;
    }
    if (slot->count == 0) {
        pthread_mutex_unlock(&slot->lock);
        return 0;
    }

    macos_ring_item_t it = slot->ring[slot->head];
    slot->head = (slot->head + 1) % MACOS_RING_CAP;
    slot->count--;
    pthread_mutex_unlock(&slot->lock);

    size_t cpy = it.len;
    if (cpy > cap) cpy = cap;
    memcpy(buf, it.data, cpy);
    return (int)cpy;
}

uint64_t hidpp_plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

void hidpp_plat_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}

uint64_t hidpp_plat_mouse_idle_ms(void) {
    CFTimeInterval secs = CGEventSourceSecondsSinceLastEventType(
        kCGEventSourceStateCombinedSessionState, kCGEventMouseMoved);
    if (isnan(secs) || secs < 0.0) return 0;
    double ms = secs * 1000.0;
    if (ms >= (double)UINT64_MAX) return UINT64_MAX - 1u;
    return (uint64_t)ms;
}

int32_t hidpp_plat_started_hidden(void) {
    int *argc_p = _NSGetArgc();
    char ***argv_p = _NSGetArgv();
    if (!argc_p || !argv_p || !*argv_p) return 0;
    int argc = *argc_p;
    char **argv = *argv_p;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--tray") == 0) return 1;
    }
    return 0;
}

int32_t hidpp_plat_can_hide_to_tray(void) {
    return 1;
}

int32_t hidpp_plat_wait_tray_ready(int32_t timeout_ms) {
    (void)timeout_ms;
    return 1;
}

int hidpp_plat_config_read(int32_t *out_dpi, int32_t *out_lod) {
    init_paths();
    return hidpp_posix_config_read_file(g_cfg_path, out_dpi, out_lod);
}

int hidpp_plat_config_write(int32_t dpi, int32_t lod) {
    init_paths();
    return hidpp_posix_config_write_file(g_cfg_path, dpi, lod);
}

static int launch_agent_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return 0;
    int n = snprintf(buf, cap, "%s/Library/LaunchAgents/com.zzj3720.mousedpi.plist", home);
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

int32_t hidpp_plat_get_autostart(void) {
    char p[PATH_MAX];
    if (!launch_agent_path(p, sizeof(p))) return 0;
    return (access(p, F_OK) == 0) ? 1 : 0;
}

int32_t hidpp_plat_set_autostart(int32_t enable) {
    init_paths();
    char p[PATH_MAX];
    if (!launch_agent_path(p, sizeof(p))) return 0;

    if (!enable) {
        (void)unlink(p);
        return hidpp_plat_get_autostart();
    }

    @autoreleasepool {
        NSString *plistPath = [NSString stringWithUTF8String:p];
        NSString *dir = [plistPath stringByDeletingLastPathComponent];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        NSString *exe = g_exe_path[0] ? [NSString stringWithUTF8String:g_exe_path] : @"";
        if (exe.length > 0) {
            NSDictionary *dict = @{
                @"Label": @"com.zzj3720.mousedpi",
                @"ProgramArguments": @[ exe, @"--tray" ],
                @"RunAtLoad": @YES,
                @"LimitLoadToSessionType": @"Aqua",
            };
            NSData *data = [NSPropertyListSerialization dataWithPropertyList:dict
                                                                      format:NSPropertyListXMLFormat_v1_0
                                                                     options:0
                                                                       error:nil];
            if (data) {
                [data writeToFile:plistPath atomically:YES];
            }
        }
    }
    return hidpp_plat_get_autostart();
}

static char *dup_abs_if_exists(const char *path) {
    if (!path || !path[0]) return NULL;
    if (access(path, R_OK) != 0) return NULL;
    char resolved[PATH_MAX];
    const char *use = realpath(path, resolved) ? resolved : path;
    return strdup(use);
}

char *hidpp_plat_picture_path_dup(void) {
    init_paths();
    @autoreleasepool {
        NSString *resPath = [[NSBundle mainBundle] resourcePath];
        if (resPath.length > 0) {
            NSString *cand = [resPath stringByAppendingPathComponent:@"mouse.png"];
            char *r = dup_abs_if_exists([cand UTF8String]);
            if (r) return r;
        }
    }
    char buf[PATH_MAX + 64];
    if (g_exe_dir[0]) {
        snprintf(buf, sizeof(buf), "%s/mouse.png", g_exe_dir);
        char *r = dup_abs_if_exists(buf);
        if (r) return r;

        snprintf(buf, sizeof(buf), "%s/../Resources/mouse.png", g_exe_dir);
        r = dup_abs_if_exists(buf);
        if (r) return r;
    }
    char *r = dup_abs_if_exists("assets/mouse.png");
    if (r) return r;
    return strdup("mouse.png");
}
