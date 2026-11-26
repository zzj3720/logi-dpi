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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void hidpp_plat_start_watchers(void) {
    /* Linux uses hid_notify_thread (0x41) + 30s refresher + Shown(away) wake restore. */
}

void hidpp_plat_stop_watchers(void) {
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

/* Query org.freedesktop.DBus.NameHasOwner("org.kde.StatusNotifierWatcher")
 * via already-linked or dynamically opened libgio-2.0.so.0 (zero glib header coupling). */
int32_t hidpp_plat_can_hide_to_tray(void) {
    void *lib = dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    void *sym_src = lib ? lib : RTLD_DEFAULT;

    typedef void *(*fn_bus_get_sync)(int, void *, void **);
    typedef void *(*fn_dbus_call_sync)(void *, const char *, const char *, const char *,
                                       const char *, void *, const void *, int, int, void *, void **);
    typedef void *(*fn_variant_new)(const char *, ...);
    typedef void  (*fn_variant_get)(void *, const char *, ...);
    typedef void  (*fn_variant_unref)(void *);
    typedef void  (*fn_object_unref)(void *);
    typedef void  (*fn_error_free)(void *);

    fn_bus_get_sync   p_bus_get   = NULL;
    fn_dbus_call_sync p_dbus_call = NULL;
    fn_variant_new    p_var_new   = NULL;
    fn_variant_get    p_var_get   = NULL;
    fn_variant_unref  p_var_unref = NULL;
    fn_object_unref   p_obj_unref = NULL;
    fn_error_free     p_err_free  = NULL;

    *(void **)(&p_bus_get)   = dlsym(sym_src, "g_bus_get_sync");
    *(void **)(&p_dbus_call) = dlsym(sym_src, "g_dbus_connection_call_sync");
    *(void **)(&p_var_new)   = dlsym(sym_src, "g_variant_new");
    *(void **)(&p_var_get)   = dlsym(sym_src, "g_variant_get");
    *(void **)(&p_var_unref) = dlsym(sym_src, "g_variant_unref");
    *(void **)(&p_obj_unref) = dlsym(sym_src, "g_object_unref");
    *(void **)(&p_err_free)  = dlsym(sym_src, "g_error_free");

    if (!p_bus_get || !p_dbus_call || !p_var_new || !p_var_get ||
        !p_var_unref || !p_obj_unref || !p_err_free) {
        if (lib) dlclose(lib);
        return 0;
    }

    int32_t has_tray = 0;
    void *err = NULL;
    /* G_BUS_TYPE_SESSION = 2 */
    void *conn = p_bus_get(2, NULL, &err);
    if (err) {
        p_err_free(err);
        err = NULL;
    }
    if (conn) {
        void *params = p_var_new("(s)", "org.kde.StatusNotifierWatcher");
        void *reply = p_dbus_call(
            conn,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "NameHasOwner",
            params,
            NULL,
            0,    /* G_DBUS_CALL_FLAGS_NONE */
            500,  /* timeout ms */
            NULL,
            &err);
        if (err) {
            p_err_free(err);
            err = NULL;
        }
        if (reply) {
            int gbool_has_owner = 0;
            p_var_get(reply, "(b)", &gbool_has_owner);
            has_tray = gbool_has_owner ? 1 : 0;
            p_var_unref(reply);
        }
        p_obj_unref(conn);
    }

    if (lib) dlclose(lib);
    return has_tray;
}

/* ---- Dynamic /sys/class/hidraw scan + HID_PHYS dedup + Report Descriptor check ---- */

typedef struct linux_cand {
    hidpp_endpoint_info_t info;
    char                  base_phys[256];
    int                   is_dj_child; /* 1 if HID_PHYS ends with ":<digits>" */
} linux_cand_t;

static int parse_uevent(const char *hidraw_name, uint16_t *out_vid, uint16_t *out_pid,
                        char *out_phys, size_t phys_cap) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", hidraw_name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int got_id = 0;
    out_phys[0] = '\0';
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
            if (phys_cap > 0) {
                snprintf(out_phys, phys_cap, "%.*s", (int)(phys_cap - 1), line + 9);
            }
        }
    }
    fclose(f);
    return got_id;
}

/* Strips trailing ":<digits>" from hid-logitech-dj child HID_PHYS (e.g. ".../input2:1"). */
static int extract_base_phys(const char *phys, char *base, size_t cap) {
    snprintf(base, cap, "%s", phys);
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

static int probe_hidraw_rdesc(const char *dev_path, int *out_perm_denied) {
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
            if (caps.has_ff00 && caps.out_bytes_11 == HIDPP_LONG_LEN) {
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
        if (!parse_uevent(de->d_name, &vid, &pid, phys, sizeof(phys))) continue;
        if (vid != HIDPP_VID_LOGITECH) continue;

        char dev_path[HIDPP_ENDPOINT_ID_MAX];
        int pn = snprintf(dev_path, sizeof(dev_path), "/dev/%s", de->d_name);
        if (pn <= 0 || (size_t)pn >= sizeof(dev_path)) continue;

        int ep_perm = 0;
        if (!probe_hidraw_rdesc(dev_path, &ep_perm)) {
            if (ep_perm) perm_denied = 1;
            continue;
        }

        char base_phys[256];
        int is_dj_child = extract_base_phys(phys, base_phys, sizeof(base_phys));

        /* Deduplicate by base HID_PHYS, preferring physical receiver over hid-logitech-dj child. */
        int dup_idx = -1;
        if (base_phys[0] != '\0') {
            for (int i = 0; i < ncand; i++) {
                if (strcmp(cands[i].base_phys, base_phys) == 0) {
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
            g_exe_path) > 0;
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
