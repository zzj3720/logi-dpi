#define _GNU_SOURCE
#include "hidpp_platform.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HIDPP_CORE_TEST
__attribute__((constructor(101)))
static void mousedpi_linux_default_env(void) {
    const char *no_id = getenv("DPI_NO_APP_ID");
    if (no_id && strcmp(no_id, "1") == 0) return;
    (void)setenv("MBUI_APP_ID", "com.zzj3720.MouseDPI", 0);
    (void)setenv("MBUI_APP_NAME", "MouseDPI", 0);
}
#endif

struct hidpp_dev {
    int fd;
};

static int  g_linux_inited = 0;
static char g_exe_path[PATH_MAX] = {0};
static char g_exe_dir[PATH_MAX]  = {0};
static char g_cfg_path[PATH_MAX] = {0};

static pthread_mutex_t g_linux_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_watch_started = 0;
static pthread_t       g_watch_thread;
static int             g_watch_pipe[2] = { -1, -1 };

static void resolve_xdg_config_dir(char *buf, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        snprintf(buf, cap, "%s", xdg);
        return;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, cap, "%s/.config", home);
        return;
    }
    buf[0] = '\0';
}

void hidpp_plat_init(void) {
    if (g_linux_inited) return;
    g_linux_inited = 1;

    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
    if (n > 0) {
        g_exe_path[n] = '\0';
        memcpy(g_exe_dir, g_exe_path, (size_t)n + 1);
        char *slash = strrchr(g_exe_dir, '/');
        if (slash) *slash = '\0';
    }

    char xdg[PATH_MAX];
    resolve_xdg_config_dir(xdg, sizeof(xdg));
    if (xdg[0]) {
        int r = snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/mousedpi/config.ini", xdg);
        if (r <= 0 || (size_t)r >= sizeof(g_cfg_path)) g_cfg_path[0] = '\0';
    }
}

static void *linux_inotify_thread_main(void *unused) {
    (void)unused;
    int in_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in_fd < 0) return NULL;

    int wd = inotify_add_watch(in_fd, "/dev", IN_CREATE | IN_DELETE);
    if (wd < 0) {
        close(in_fd);
        return NULL;
    }

    int wake_fd = -1;
    pthread_mutex_lock(&g_linux_watch_lock);
    wake_fd = g_watch_pipe[0];
    pthread_mutex_unlock(&g_linux_watch_lock);

    while (!hidpp_core_is_stopping()) {
        struct pollfd pfds[2];
        pfds[0].fd = in_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = wake_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int prc = poll(pfds, wake_fd >= 0 ? 2 : 1, 500);
        if (prc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (prc == 0) continue;
        if (wake_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            break;
        }
        if (pfds[0].revents & POLLIN) {
            uint8_t buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            int hit = 0;
            for (;;) {
                ssize_t n = read(in_fd, buf, sizeof(buf));
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    break;
                }
                ssize_t off = 0;
                while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
                    const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
                    if (ev->len > 0 && strncmp(ev->name, "hidraw", 6) == 0) {
                        hit = 1;
                    }
                    off += (ssize_t)sizeof(struct inotify_event) + (ssize_t)ev->len;
                }
            }
            if (hit) {
                hidpp_core_signal_topology_change();
            }
        }
    }

    (void)inotify_rm_watch(in_fd, wd);
    close(in_fd);
    return NULL;
}

void hidpp_plat_start_watchers(void) {
    pthread_mutex_lock(&g_linux_watch_lock);
    if (g_watch_started || hidpp_core_is_stopping()) {
        pthread_mutex_unlock(&g_linux_watch_lock);
        return;
    }
    if (pipe2(g_watch_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        g_watch_pipe[0] = -1;
        g_watch_pipe[1] = -1;
        pthread_mutex_unlock(&g_linux_watch_lock);
        return;
    }
    if (pthread_create(&g_watch_thread, NULL, linux_inotify_thread_main, NULL) == 0) {
        g_watch_started = 1;
    } else {
        close(g_watch_pipe[0]);
        close(g_watch_pipe[1]);
        g_watch_pipe[0] = -1;
        g_watch_pipe[1] = -1;
    }
    pthread_mutex_unlock(&g_linux_watch_lock);
}

void hidpp_plat_stop_watchers(void) {
    pthread_mutex_lock(&g_linux_watch_lock);
    if (!g_watch_started) {
        pthread_mutex_unlock(&g_linux_watch_lock);
        return;
    }
    g_watch_started = 0;
    pthread_t th = g_watch_thread;
    int wfd = g_watch_pipe[1];
    pthread_mutex_unlock(&g_linux_watch_lock);

    if (wfd >= 0) {
        uint8_t b = 1;
        ssize_t ignored = write(wfd, &b, 1);
        (void)ignored;
    }
    pthread_join(th, NULL);

    pthread_mutex_lock(&g_linux_watch_lock);
    if (g_watch_pipe[0] >= 0) {
        close(g_watch_pipe[0]);
        g_watch_pipe[0] = -1;
    }
    if (g_watch_pipe[1] >= 0) {
        close(g_watch_pipe[1]);
        g_watch_pipe[1] = -1;
    }
    pthread_mutex_unlock(&g_linux_watch_lock);
}

int hidpp_plat_needs_notify_thread(void) {
    return 1;
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

/* Linux does not track global pointer idle time; disable 2s gentle polling. */
uint64_t hidpp_plat_mouse_idle_ms(void) {
    return UINT64_MAX;
}

/* Parse /proc/self/cmdline (NUL-separated argv) for exact "--tray" token. */
int32_t hidpp_plat_started_hidden(void) {
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    int arg_idx = 0;
    ssize_t i = 0;
    while (i < n) {
        const char *tok = buf + i;
        size_t tlen = strlen(tok);
        if (arg_idx >= 1 && strcmp(tok, "--tray") == 0) {
            return 1;
        }
        arg_idx++;
        i += (ssize_t)tlen + 1;
    }
    return 0;
}

/* ---- D-Bus SNI tray availability & registration verification via libgio-2.0.so.0 ---- */

typedef void  *(*gio_bus_get_sync_fn)(int, void *, void **);
typedef void  *(*gio_dbus_call_sync_fn)(void *, const char *, const char *, const char *,
                                        const char *, void *, const void *, int, int, void *, void **);
typedef void  *(*gio_var_new_fn)(const char *, ...);
typedef void   (*gio_var_get_fn)(void *, const char *, ...);
typedef const char *(*gio_var_type_str_fn)(void *);
typedef int    (*gio_var_get_bool_fn)(void *);
typedef size_t (*gio_var_n_children_fn)(void *);
typedef void  *(*gio_var_get_child_fn)(void *, size_t);
typedef const char *(*gio_var_get_str_fn)(void *, size_t *);
typedef void   (*gio_var_unref_fn)(void *);
typedef void   (*gio_obj_unref_fn)(void *);
typedef void   (*gio_err_free_fn)(void *);

typedef struct gio_dbus_syms {
    void                  *lib;
    gio_bus_get_sync_fn    bus_get;
    gio_dbus_call_sync_fn  dbus_call;
    gio_var_new_fn         var_new;
    gio_var_get_fn         var_get;
    gio_var_type_str_fn    var_type_str;
    gio_var_get_bool_fn    var_get_bool;
    gio_var_n_children_fn  var_n_children;
    gio_var_get_child_fn   var_get_child;
    gio_var_get_str_fn     var_get_str;
    gio_var_unref_fn       var_unref;
    gio_obj_unref_fn       obj_unref;
    gio_err_free_fn        err_free;
} gio_dbus_syms_t;

static int gio_dbus_load(gio_dbus_syms_t *g) {
    memset(g, 0, sizeof(*g));
    g->lib = dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    void *sym_src = g->lib ? g->lib : RTLD_DEFAULT;

    *(void **)(&g->bus_get)        = dlsym(sym_src, "g_bus_get_sync");
    *(void **)(&g->dbus_call)      = dlsym(sym_src, "g_dbus_connection_call_sync");
    *(void **)(&g->var_new)        = dlsym(sym_src, "g_variant_new");
    *(void **)(&g->var_get)        = dlsym(sym_src, "g_variant_get");
    *(void **)(&g->var_type_str)   = dlsym(sym_src, "g_variant_get_type_string");
    *(void **)(&g->var_get_bool)   = dlsym(sym_src, "g_variant_get_boolean");
    *(void **)(&g->var_n_children) = dlsym(sym_src, "g_variant_n_children");
    *(void **)(&g->var_get_child)  = dlsym(sym_src, "g_variant_get_child_value");
    *(void **)(&g->var_get_str)    = dlsym(sym_src, "g_variant_get_string");
    *(void **)(&g->var_unref)      = dlsym(sym_src, "g_variant_unref");
    *(void **)(&g->obj_unref)      = dlsym(sym_src, "g_object_unref");
    *(void **)(&g->err_free)       = dlsym(sym_src, "g_error_free");

    if (!g->bus_get || !g->dbus_call || !g->var_new || !g->var_get ||
        !g->var_type_str || !g->var_get_bool || !g->var_n_children || !g->var_get_child ||
        !g->var_get_str || !g->var_unref || !g->obj_unref || !g->err_free) {
        if (g->lib) {
            dlclose(g->lib);
            g->lib = NULL;
        }
        return 0;
    }
    return 1;
}

static void gio_dbus_unload(gio_dbus_syms_t *g) {
    if (g->lib) {
        dlclose(g->lib);
        g->lib = NULL;
    }
}

static int dbus_sni_host_registered(const gio_dbus_syms_t *g, void *conn) {
    void *err = NULL;
    void *params = g->var_new("(s)", "org.kde.StatusNotifierWatcher");
    void *reply = g->dbus_call(
        conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        params,
        NULL,
        0,
        500,
        NULL,
        &err);
    if (err) {
        g->err_free(err);
        err = NULL;
    }
    int has_owner = 0;
    if (reply) {
        int b = 0;
        g->var_get(reply, "(b)", &b);
        has_owner = b ? 1 : 0;
        g->var_unref(reply);
    }
    if (!has_owner) return 0;

    void *prop_params = g->var_new(
        "(ss)",
        "org.kde.StatusNotifierWatcher",
        "IsStatusNotifierHostRegistered");
    void *prop_reply = g->dbus_call(
        conn,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties",
        "Get",
        prop_params,
        NULL,
        0,
        500,
        NULL,
        &err);
    if (err) {
        g->err_free(err);
        err = NULL;
    }
    int host_ok = 0;
    if (prop_reply) {
        void *inner = NULL;
        g->var_get(prop_reply, "(v)", &inner);
        if (inner) {
            const char *ts = g->var_type_str(inner);
            if (ts && strcmp(ts, "b") == 0) {
                host_ok = g->var_get_bool(inner) ? 1 : 0;
            }
            g->var_unref(inner);
        }
        g->var_unref(prop_reply);
    }
    return host_ok;
}

int32_t hidpp_plat_can_hide_to_tray(void) {
    gio_dbus_syms_t g;
    if (!gio_dbus_load(&g)) return 0;

    int32_t has_tray = 0;
    void *err = NULL;
    /* G_BUS_TYPE_SESSION = 2 */
    void *conn = g.bus_get(2, NULL, &err);
    if (err) {
        g.err_free(err);
        err = NULL;
    }
    if (conn) {
        has_tray = dbus_sni_host_registered(&g, conn) ? 1 : 0;
        g.obj_unref(conn);
    }
    gio_dbus_unload(&g);
    return has_tray;
}

int32_t hidpp_plat_wait_tray_ready(int32_t timeout_ms) {
    gio_dbus_syms_t g;
    if (!gio_dbus_load(&g)) return 0;

    void *err = NULL;
    void *conn = g.bus_get(2, NULL, &err);
    if (err) {
        g.err_free(err);
        err = NULL;
    }
    if (!conn) {
        gio_dbus_unload(&g);
        return 0;
    }

    if (!dbus_sni_host_registered(&g, conn)) {
        g.obj_unref(conn);
        gio_dbus_unload(&g);
        return 0;
    }

    char needle[128];
    snprintf(needle, sizeof(needle), "org.kde.StatusNotifierItem-%ld-1", (long)getpid());

    uint64_t deadline = hidpp_plat_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    int32_t ready = 0;

    for (;;) {
        if (hidpp_core_is_stopping()) break;

        void *params = g.var_new(
            "(ss)",
            "org.kde.StatusNotifierWatcher",
            "RegisteredStatusNotifierItems");
        void *reply = g.dbus_call(
            conn,
            "org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher",
            "org.freedesktop.DBus.Properties",
            "Get",
            params,
            NULL,
            0,
            500,
            NULL,
            &err);
        if (err) {
            g.err_free(err);
            err = NULL;
        }
        int found = 0;
        if (reply) {
            void *inner = NULL;
            g.var_get(reply, "(v)", &inner);
            if (inner) {
                const char *ts = g.var_type_str(inner);
                if (ts && strcmp(ts, "as") == 0) {
                    size_t n = g.var_n_children(inner);
                    for (size_t i = 0; i < n; i++) {
                        void *child = g.var_get_child(inner, i);
                        if (child) {
                            const char *s = g.var_get_str(child, NULL);
                            if (s && strstr(s, needle) != NULL) {
                                found = 1;
                            }
                            g.var_unref(child);
                        }
                        if (found) break;
                    }
                }
                g.var_unref(inner);
            }
            g.var_unref(reply);
        }

        if (found && dbus_sni_host_registered(&g, conn)) {
            ready = 1;
            break;
        }

        uint64_t now = hidpp_plat_now_ms();
        if (now >= deadline) break;
        uint64_t rem = deadline - now;
        hidpp_plat_sleep_ms(rem > 50u ? 50u : (uint32_t)rem);
    }

    g.obj_unref(conn);
    gio_dbus_unload(&g);
    return ready;
}

/* ---- Dynamic /sys/class/hidraw scan + HID_PHYS dedup + Report Descriptor check ---- */

typedef struct linux_cand {
    hidpp_endpoint_info_t info;
    char                  base_phys[256];
    char                  uniq[128];
    int                   is_usb;
    int                   is_dj_child; /* 1 if USB HID_PHYS ends with ":<digits>" */
} linux_cand_t;

static int parse_uevent(const char *hidraw_name, uint16_t *out_vid, uint16_t *out_pid,
                        char *out_phys, size_t phys_cap,
                        char *out_uniq, size_t uniq_cap) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", hidraw_name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int got_id = 0;
    if (out_phys && phys_cap > 0) out_phys[0] = '\0';
    if (out_uniq && uniq_cap > 0) out_uniq[0] = '\0';
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (strncmp(line, "HID_ID=", 7) == 0) {
            unsigned int bus = 0, vid = 0, pid = 0;
            if (sscanf(line + 7, "%x:%x:%x", &bus, &vid, &pid) == 3) {
                *out_vid = (uint16_t)vid;
                *out_pid = (uint16_t)pid;
                got_id = 1;
            }
        } else if (strncmp(line, "HID_PHYS=", 9) == 0) {
            if (out_phys && phys_cap > 0) {
                snprintf(out_phys, phys_cap, "%.*s", (int)(phys_cap - 1), line + 9);
            }
        } else if (strncmp(line, "HID_UNIQ=", 9) == 0) {
            if (out_uniq && uniq_cap > 0) {
                snprintf(out_uniq, uniq_cap, "%.*s", (int)(uniq_cap - 1), line + 9);
            }
        }
    }
    fclose(f);
    return got_id;
}

/* Strips trailing ":<digits>" ONLY from USB hid-logitech-dj child HID_PHYS (e.g. "usb-.../input2:1").
 * Never treats Bluetooth MAC addresses (aa:bb:cc:dd:ee:ff) as DJ children. */
static int extract_base_phys(const char *phys, char *base, size_t cap) {
    snprintf(base, cap, "%s", phys);
    if (strncmp(phys, "usb-", 4) != 0) {
        return 0;
    }
    char *colon = strrchr(base, ':');
    if (colon && colon[1] != '\0') {
        int all_digits = 1;
        for (const char *p = colon + 1; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) {
            *colon = '\0';
            return 1; /* is_dj_child */
        }
    }
    return 0;
}

static int probe_hidraw_rdesc(const char *hidraw_name, const char *dev_path, int *out_perm_denied) {
    /* First read world-readable sysfs report_descriptor so non-HID++ endpoints never
     * trigger false EACCES permission errors on /dev/hidrawX. */
    char sys_path[PATH_MAX];
    snprintf(sys_path, sizeof(sys_path), "/sys/class/hidraw/%s/device/report_descriptor", hidraw_name);
    int sfd = open(sys_path, O_RDONLY | O_CLOEXEC);
    if (sfd >= 0) {
        uint8_t rbuf[HID_MAX_DESCRIPTOR_SIZE];
        ssize_t n = read(sfd, rbuf, sizeof(rbuf));
        close(sfd);
        if (n <= 0) return 0;

        hidpp_rdesc_caps_t caps;
        hidpp_parse_report_descriptor(rbuf, (size_t)n, &caps);
        if (!(caps.has_ff00 &&
              caps.out_bytes_11 == HIDPP_LONG_LEN &&
              caps.in_bytes_11 >= HIDPP_LONG_LEN)) {
            return 0;
        }

        int fd = open(dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM) {
                if (out_perm_denied) *out_perm_denied = 1;
            }
            return 0;
        }
        close(fd);
        return 1;
    }

    if (errno != ENOENT) {
        return 0;
    }

    /* Fallback when sysfs report_descriptor is absent */
    int fd = open(dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            if (out_perm_denied) *out_perm_denied = 1;
        }
        return 0;
    }

    int rdesc_size = 0;
    int match = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &rdesc_size) >= 0 &&
        rdesc_size > 0 && rdesc_size <= HID_MAX_DESCRIPTOR_SIZE) {
        struct hidraw_report_descriptor rdesc;
        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.size = (unsigned int)rdesc_size;
        if (ioctl(fd, HIDIOCGRDESC, &rdesc) >= 0) {
            hidpp_rdesc_caps_t caps;
            hidpp_parse_report_descriptor(rdesc.value, rdesc.size, &caps);
            if (caps.has_ff00 &&
                caps.out_bytes_11 == HIDPP_LONG_LEN &&
                caps.in_bytes_11 >= HIDPP_LONG_LEN) {
                match = 1;
            }
        }
    }
    close(fd);
    return match;
}

static int cmp_linux_cand(const void *a, const void *b) {
    const linux_cand_t *ca = (const linux_cand_t *)a;
    const linux_cand_t *cb = (const linux_cand_t *)b;
    if (ca->info.kind < cb->info.kind) return -1;
    if (ca->info.kind > cb->info.kind) return 1;
    if (ca->is_dj_child < cb->is_dj_child) return -1;
    if (ca->is_dj_child > cb->is_dj_child) return 1;
    return strcmp(ca->info.id, cb->info.id);
}

int hidpp_plat_enumerate(hidpp_endpoint_info_t *out, int max_count, int *out_perm_denied) {
    if (out_perm_denied) *out_perm_denied = 0;
    if (!out || max_count <= 0) return 0;

    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return 0;

    linux_cand_t cands[HIDPP_MAX_ENDPOINTS];
    int ncand = 0;
    int perm_denied = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "hidraw", 6) != 0) continue;

        uint16_t vid = 0, pid = 0;
        char phys[256];
        char uniq[128];
        if (!parse_uevent(de->d_name, &vid, &pid, phys, sizeof(phys), uniq, sizeof(uniq))) continue;
        if (vid != HIDPP_VID_LOGITECH) continue;

        char dev_path[HIDPP_ENDPOINT_ID_MAX];
        int pn = snprintf(dev_path, sizeof(dev_path), "/dev/%s", de->d_name);
        if (pn <= 0 || (size_t)pn >= sizeof(dev_path)) continue;

        int ep_perm = 0;
        if (!probe_hidraw_rdesc(de->d_name, dev_path, &ep_perm)) {
            if (ep_perm) perm_denied = 1;
            continue;
        }

        char base_phys[256];
        int is_usb = (strncmp(phys, "usb-", 4) == 0) ? 1 : 0;
        int is_dj_child = extract_base_phys(phys, base_phys, sizeof(base_phys));

        /* Only deduplicate USB hid-logitech-dj child vs parent receiver on the same base_phys.
         * Never merge two Bluetooth devices or two independent non-DJ USB devices. */
        int dup_idx = -1;
        if (is_usb && base_phys[0] != '\0') {
            for (int i = 0; i < ncand; i++) {
                if (cands[i].is_usb &&
                    (is_dj_child || cands[i].is_dj_child) &&
                    strcmp(cands[i].base_phys, base_phys) == 0) {
                    dup_idx = i;
                    break;
                }
            }
        }

        if (dup_idx >= 0) {
            if (cands[dup_idx].is_dj_child && !is_dj_child) {
                memcpy(cands[dup_idx].info.id, dev_path, sizeof(cands[dup_idx].info.id));
                cands[dup_idx].info.id[sizeof(cands[dup_idx].info.id) - 1] = '\0';
                cands[dup_idx].info.vid = vid;
                cands[dup_idx].info.pid = pid;
                cands[dup_idx].info.kind = hidpp_classify_pid(pid);
                memcpy(cands[dup_idx].uniq, uniq, sizeof(cands[dup_idx].uniq));
                cands[dup_idx].uniq[sizeof(cands[dup_idx].uniq) - 1] = '\0';
                cands[dup_idx].is_dj_child = 0;
            }
            continue;
        }

        if (ncand < HIDPP_MAX_ENDPOINTS) {
            linux_cand_t *c = &cands[ncand++];
            memset(c, 0, sizeof(*c));
            memcpy(c->info.id, dev_path, sizeof(c->info.id));
            c->info.id[sizeof(c->info.id) - 1] = '\0';
            c->info.vid = vid;
            c->info.pid = pid;
            c->info.kind = hidpp_classify_pid(pid);
            memcpy(c->base_phys, base_phys, sizeof(c->base_phys));
            c->base_phys[sizeof(c->base_phys) - 1] = '\0';
            memcpy(c->uniq, uniq, sizeof(c->uniq));
            c->uniq[sizeof(c->uniq) - 1] = '\0';
            c->is_usb = is_usb;
            c->is_dj_child = is_dj_child;
        }
    }
    closedir(dir);

    if (ncand > 1) {
        qsort(cands, (size_t)ncand, sizeof(linux_cand_t), cmp_linux_cand);
    }

    int ret = ncand < max_count ? ncand : max_count;
    for (int i = 0; i < ret; i++) {
        out[i] = cands[i].info;
    }
    if (out_perm_denied) *out_perm_denied = perm_denied;
    return ret;
}

hidpp_dev_t *hidpp_plat_open(const char *endpoint_id, int notify_only, int *out_perm_denied) {
    if (out_perm_denied) *out_perm_denied = 0;
    if (!endpoint_id || !endpoint_id[0]) return NULL;

    int flags = (notify_only ? O_RDONLY : O_RDWR) | O_NONBLOCK | O_CLOEXEC;
    int fd = open(endpoint_id, flags);
    if (fd < 0) {
        if ((errno == EACCES || errno == EPERM) && out_perm_denied) {
            *out_perm_denied = 1;
        }
        return NULL;
    }

    hidpp_dev_t *dev = (hidpp_dev_t *)malloc(sizeof(*dev));
    if (!dev) {
        close(fd);
        return NULL;
    }
    dev->fd = fd;
    return dev;
}

void hidpp_plat_close(hidpp_dev_t *dev) {
    if (!dev) return;
    if (dev->fd >= 0) close(dev->fd);
    free(dev);
}

void hidpp_plat_drain(hidpp_dev_t *dev) {
    if (!dev || dev->fd < 0) return;
    uint8_t sink[64];
    for (int i = 0; i < 64; i++) {
        ssize_t n = read(dev->fd, sink, sizeof(sink));
        if (n <= 0) break;
    }
}

int hidpp_plat_write(hidpp_dev_t *dev, const uint8_t *buf, size_t len) {
    if (!dev || dev->fd < 0 || !buf || len == 0) return -1;
    for (;;) {
        ssize_t n = write(dev->fd, buf, len);
        if (n < 0 && errno == EINTR) continue;
        return (int)n;
    }
}

int hidpp_plat_read(hidpp_dev_t *dev, uint8_t *buf, size_t cap, int timeout_ms) {
    if (!dev || dev->fd < 0 || !buf || cap == 0) return -1;

    uint64_t deadline = (timeout_ms > 0) ? (hidpp_plat_now_ms() + (uint64_t)timeout_ms) : 0;
    for (;;) {
        int wait_ms = 0;
        if (timeout_ms < 0) {
            wait_ms = -1;
        } else if (timeout_ms > 0) {
            uint64_t now = hidpp_plat_now_ms();
            if (now >= deadline) return 0;
            wait_ms = (int)(deadline - now);
        }

        struct pollfd pfd;
        pfd.fd = dev->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int prc = poll(&pfd, 1, wait_ms);
        if (prc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (prc == 0) return 0;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;

        ssize_t n = read(dev->fd, buf, cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (timeout_ms == 0) return 0;
                continue;
            }
            return -1;
        }
        return (int)n;
    }
}

/* ---- Config (POSIX atomic config.ini via hidpp_core.c) ---- */

int hidpp_plat_config_read(int32_t *out_dpi, int32_t *out_lod) {
    hidpp_plat_init();
    return hidpp_posix_config_read_file(g_cfg_path, out_dpi, out_lod);
}

int hidpp_plat_config_write(int32_t dpi, int32_t lod) {
    hidpp_plat_init();
    return hidpp_posix_config_write_file(g_cfg_path, dpi, lod);
}

/* ---- XDG autostart ---- */

static int autostart_desktop_path(char *buf, size_t cap) {
    char xdg[PATH_MAX];
    resolve_xdg_config_dir(xdg, sizeof(xdg));
    if (!xdg[0]) return 0;
    int n = snprintf(buf, cap, "%s/autostart/com.zzj3720.MouseDPI.desktop", xdg);
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

static int mkdir_p_dir(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return 0;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return 0;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return 0;
    return 1;
}

int32_t hidpp_plat_get_autostart(void) {
    char p[PATH_MAX];
    if (!autostart_desktop_path(p, sizeof(p))) return 0;
    return (access(p, F_OK) == 0) ? 1 : 0;
}

static int escape_desktop_exec_arg(const char *src, char *dst, size_t cap) {
    if (!src || !dst || cap == 0) return 0;
    size_t pos = 0;
    for (const char *p = src; *p; p++) {
        char c = *p;
        if (c == '%') {
            if (pos + 2 >= cap) return 0;
            dst[pos++] = '%';
            dst[pos++] = '%';
        } else if (c == '\\' || c == '"' || c == '`' || c == '$') {
            if (pos + 2 >= cap) return 0;
            dst[pos++] = '\\';
            dst[pos++] = c;
        } else {
            if (pos + 1 >= cap) return 0;
            dst[pos++] = c;
        }
    }
    dst[pos] = '\0';
    return 1;
}

int32_t hidpp_plat_set_autostart(int32_t enable) {
    hidpp_plat_init();
    char p[PATH_MAX];
    if (!autostart_desktop_path(p, sizeof(p))) return 0;

    if (!enable) {
        (void)unlink(p);
        return hidpp_plat_get_autostart();
    }

    char dir[PATH_MAX];
    memcpy(dir, p, sizeof(dir));
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)mkdir_p_dir(dir);
    }

    char escaped_exe[PATH_MAX * 2];
    if (!escape_desktop_exec_arg(g_exe_path, escaped_exe, sizeof(escaped_exe))) {
        return hidpp_plat_get_autostart();
    }

    char tmp[PATH_MAX + 32];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", p, (long)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp)) return hidpp_plat_get_autostart();

    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=MouseDPI\n"
            "Exec=\"%s\" --tray\n"
            "X-GNOME-Autostart-enabled=true\n",
            escaped_exe) > 0;
        if (ok && fflush(f) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok) {
            if (rename(tmp, p) != 0) (void)unlink(tmp);
        } else {
            (void)unlink(tmp);
        }
    }
    return hidpp_plat_get_autostart();
}

/* ---- Picture path ---- */

static char *dup_abs_if_exists(const char *path) {
    if (!path || !path[0]) return NULL;
    if (access(path, R_OK) != 0) return NULL;
    char resolved[PATH_MAX];
    const char *use = realpath(path, resolved) ? resolved : path;
    return strdup(use);
}

char *hidpp_plat_picture_path_dup(void) {
    hidpp_plat_init();
    char buf[PATH_MAX + 64];
    if (g_exe_dir[0]) {
        snprintf(buf, sizeof(buf), "%s/mouse.png", g_exe_dir);
        char *r = dup_abs_if_exists(buf);
        if (r) return r;

        snprintf(buf, sizeof(buf), "%s/assets/mouse.png", g_exe_dir);
        r = dup_abs_if_exists(buf);
        if (r) return r;

        snprintf(buf, sizeof(buf), "%s/../share/mousedpi/mouse.png", g_exe_dir);
        r = dup_abs_if_exists(buf);
        if (r) return r;
    }
    char *r = dup_abs_if_exists("assets/mouse.png");
    if (r) return r;
    return strdup("mouse.png");
}
