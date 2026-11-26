#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hidpp_platform.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---- Cross-platform mutex / condvar / thread primitives ---- */

#if defined(_WIN32)
typedef CRITICAL_SECTION   hidpp_mutex_t;
typedef CONDITION_VARIABLE hidpp_cond_t;
typedef HANDLE             hidpp_thread_t;

static void hidpp_mutex_init(hidpp_mutex_t *m) { InitializeCriticalSection(m); }
static void hidpp_mutex_lock(hidpp_mutex_t *m) { EnterCriticalSection(m); }
static void hidpp_mutex_unlock(hidpp_mutex_t *m) { LeaveCriticalSection(m); }

static void hidpp_cond_init(hidpp_cond_t *c) { InitializeConditionVariable(c); }
static void hidpp_cond_broadcast(hidpp_cond_t *c) { WakeAllConditionVariable(c); }
static void hidpp_cond_wait(hidpp_cond_t *c, hidpp_mutex_t *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
static int hidpp_cond_timedwait_ms(hidpp_cond_t *c, hidpp_mutex_t *m, uint32_t ms) {
    return SleepConditionVariableCS(c, m, (DWORD)ms) ? 0 : 1;
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} hidpp_win_th_arg_t;

static DWORD WINAPI hidpp_win_th_trampoline(LPVOID p) {
    hidpp_win_th_arg_t *a = (hidpp_win_th_arg_t *)p;
    void (*fn)(void *) = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return 0;
}

static int hidpp_thread_start(hidpp_thread_t *t, void (*fn)(void *), void *arg) {
    hidpp_win_th_arg_t *a = (hidpp_win_th_arg_t *)malloc(sizeof(*a));
    if (!a) return 0;
    a->fn = fn;
    a->arg = arg;
    HANDLE h = CreateThread(NULL, 0, hidpp_win_th_trampoline, a, 0, NULL);
    if (!h) {
        free(a);
        return 0;
    }
    *t = h;
    return 1;
}

static void hidpp_thread_join(hidpp_thread_t t) {
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}
#else
typedef pthread_mutex_t hidpp_mutex_t;
typedef pthread_cond_t  hidpp_cond_t;
typedef pthread_t       hidpp_thread_t;

static void hidpp_mutex_init(hidpp_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void hidpp_mutex_lock(hidpp_mutex_t *m) { pthread_mutex_lock(m); }
static void hidpp_mutex_unlock(hidpp_mutex_t *m) { pthread_mutex_unlock(m); }

static void hidpp_cond_init(hidpp_cond_t *c) {
#if defined(__linux__)
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
#else
    pthread_cond_init(c, NULL);
#endif
}
static void hidpp_cond_broadcast(hidpp_cond_t *c) { pthread_cond_broadcast(c); }
static void hidpp_cond_wait(hidpp_cond_t *c, hidpp_mutex_t *m) { pthread_cond_wait(c, m); }

static int hidpp_cond_timedwait_ms(hidpp_cond_t *c, hidpp_mutex_t *m, uint32_t ms) {
    struct timespec ts;
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    return pthread_cond_timedwait_relative_np(c, m, &ts) == ETIMEDOUT ? 1 : 0;
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)((ms % 1000u) * 1000000u);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? 1 : 0;
#endif
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} hidpp_posix_th_arg_t;

static void *hidpp_posix_th_trampoline(void *p) {
    hidpp_posix_th_arg_t *a = (hidpp_posix_th_arg_t *)p;
    void (*fn)(void *) = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return NULL;
}

static int hidpp_thread_start(hidpp_thread_t *t, void (*fn)(void *), void *arg) {
    hidpp_posix_th_arg_t *a = (hidpp_posix_th_arg_t *)malloc(sizeof(*a));
    if (!a) return 0;
    a->fn = fn;
    a->arg = arg;
    if (pthread_create(t, NULL, hidpp_posix_th_trampoline, a) != 0) {
        free(a);
        return 0;
    }
    return 1;
}

static void hidpp_thread_join(hidpp_thread_t t) {
    pthread_join(t, NULL);
}
#endif

/* ---- HID Report Descriptor parser (USB HID 1.11) ---- */

void hidpp_parse_report_descriptor(const uint8_t *desc, size_t len, hidpp_rdesc_caps_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!desc || len == 0) return;

    typedef struct {
        uint32_t usage_page;
        uint32_t report_size;
        uint32_t report_count;
        uint32_t report_id;
    } hid_global_t;

    hid_global_t cur;
    memset(&cur, 0, sizeof(cur));
    hid_global_t stack[8];
    int sp = 0;

    uint64_t in_bits_10 = 0;
    uint64_t out_bits_10 = 0;
    uint64_t in_bits_11 = 0;
    uint64_t out_bits_11 = 0;

    size_t pos = 0;
    while (pos < len) {
        uint8_t b0 = desc[pos];
        if (b0 == 0xFEu) {
            /* Long item: [0xFE, data_size, long_item_tag, data...] */
            if (pos + 2 >= len) break;
            size_t lsize = desc[pos + 1];
            if (pos + 3 + lsize > len) break;
            pos += 3 + lsize;
            continue;
        }
        uint8_t bsize = b0 & 0x03u;
        size_t size = (bsize == 3u) ? 4u : (size_t)bsize;
        uint8_t btype = (b0 >> 2) & 0x03u;
        uint8_t btag  = (b0 >> 4) & 0x0Fu;

        if (pos + 1 + size > len) break;

        uint32_t val = 0;
        for (size_t i = 0; i < size; i++) {
            val |= ((uint32_t)desc[pos + 1 + i]) << (8u * i);
        }
        pos += 1 + size;

        if (btype == 1u) {
            /* Global item */
            switch (btag) {
                case 0x0u: /* Usage Page */
                    cur.usage_page = val;
                    if (val == HIDPP_USAGE_PAGE_VENDOR) out->has_ff00 = 1;
                    break;
                case 0x7u: /* Report Size */
                    cur.report_size = val;
                    break;
                case 0x8u: /* Report ID */
                    cur.report_id = val;
                    break;
                case 0x9u: /* Report Count */
                    cur.report_count = val;
                    break;
                case 0xAu: /* Push */
                    if (sp < 8) stack[sp++] = cur;
                    break;
                case 0xBu: /* Pop */
                    if (sp > 0) cur = stack[--sp];
                    break;
                default:
                    break;
            }
        } else if (btype == 0u) {
            /* Main item */
            uint64_t bits = (uint64_t)cur.report_size * (uint64_t)cur.report_count;
            if (btag == 0x8u) {
                /* Input */
                if (cur.report_id == HIDPP_SHORT_REPORT_ID) {
                    in_bits_10 += bits;
                } else if (cur.report_id == HIDPP_LONG_REPORT_ID) {
                    in_bits_11 += bits;
                }
            } else if (btag == 0x9u) {
                /* Output */
                if (cur.report_id == HIDPP_SHORT_REPORT_ID) {
                    out_bits_10 += bits;
                } else if (cur.report_id == HIDPP_LONG_REPORT_ID) {
                    out_bits_11 += bits;
                }
            } else if (btag == 0xAu) {
                /* Collection */
                if (cur.usage_page == HIDPP_USAGE_PAGE_VENDOR) out->has_ff00 = 1;
            }
        }
    }

    if (in_bits_10 > 0)  out->in_bytes_10  = (uint32_t)(1u + (in_bits_10 + 7u) / 8u);
    if (out_bits_10 > 0) out->out_bytes_10 = (uint32_t)(1u + (out_bits_10 + 7u) / 8u);
    if (in_bits_11 > 0)  out->in_bytes_11  = (uint32_t)(1u + (in_bits_11 + 7u) / 8u);
    if (out_bits_11 > 0) out->out_bytes_11 = (uint32_t)(1u + (out_bits_11 + 7u) / 8u);
}

/* ---- POSIX atomic config.ini read/write ---- */

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
    return s;
}

static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int hidpp_posix_config_read_file(const char *path, int32_t *out_dpi, int32_t *out_lod) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int in_mouse = 0;
    int got_dpi = 0;
    int32_t dpi = 0;
    int32_t lod = HIDPP_DEFAULT_LOD;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim_ws(line);
        if (*p == '\0' || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                in_mouse = eq_ci(trim_ws(p + 1), "mouse");
            }
            continue;
        }
        if (!in_mouse) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_ws(p);
        char *v = trim_ws(eq + 1);
        if (eq_ci(k, "dpi")) {
            char *ep = NULL;
            long val = strtol(v, &ep, 10);
            if (ep != v && val >= HIDPP_MIN_DPI && val <= HIDPP_MAX_DPI) {
                dpi = (int32_t)val;
                got_dpi = 1;
            }
        } else if (eq_ci(k, "lod")) {
            char *ep = NULL;
            long val = strtol(v, &ep, 10);
            if (ep != v && val >= 1 && val <= 3) {
                lod = (int32_t)val;
            }
        }
    }
    fclose(f);
    if (!got_dpi) return 0;
    if (out_dpi) *out_dpi = dpi;
    if (out_lod) *out_lod = lod;
    return 1;
}

#if !defined(_WIN32)
static int mkdir_p_parent(const char *file_path) {
    char buf[1024];
    size_t len = strlen(file_path);
    if (len == 0 || len >= sizeof(buf)) return 0;
    memcpy(buf, file_path, len + 1);

    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 1;
    *slash = '\0';

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
#endif

int hidpp_posix_config_write_file(const char *path, int32_t dpi, int32_t lod) {
    if (!path || !path[0]) return 0;
#if !defined(_WIN32)
    if (!mkdir_p_parent(path)) return 0;
#endif
    char tmp[1100];
#if !defined(_WIN32)
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
#else
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
#endif
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return 0;

    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fprintf(f, "[mouse]\ndpi=%d\nlod=%d\n", (int)dpi, (int)lod) > 0;
    if (ok && fflush(f) != 0) ok = 0;
#if !defined(_WIN32)
    if (ok) {
        int fd = fileno(f);
        if (fd >= 0) (void)fsync(fd);
    }
#endif
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return 0;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

/* ---- Core state & synchronization ---- */

typedef struct hidpp_state {
    int32_t rev;
    int32_t applied_seq;
    int32_t connected;
    int32_t perm_denied;
    int32_t dpi_x;
    int32_t dpi_y;
    int32_t lod;
    int32_t battery;
    int32_t mode;
    char    device_name[64];
} hidpp_state_t;

static hidpp_mutex_t  g_life_lock;
static hidpp_cond_t   g_stop_cv;
static int            g_stop_claimed = 0;
static int            g_stop_done = 0;
static hidpp_mutex_t  g_hid_lock;
static hidpp_mutex_t  g_snap_lock;
static hidpp_cond_t   g_rev_cv;

static hidpp_mutex_t  g_wake_lock;
static hidpp_cond_t   g_wake_cv;
static int            g_wake_burst = 0;

static hidpp_mutex_t  g_refresh_lock;
static hidpp_cond_t   g_refresh_cv;
static int            g_refresh_pending = 0;
static int            g_want_pending = 0;
static int32_t        g_want_dpi = HIDPP_DEFAULT_DPI;
static int32_t        g_want_lod = HIDPP_DEFAULT_LOD;
static int32_t        g_want_seq = 0;

static int            g_stopping = 0;
static int            g_topology_dirty = 0;
static int            g_feat_cache_dirty = 0;
static int            g_watchers_started = 0;
static hidpp_thread_t g_th_wake;
static int            g_th_wake_valid = 0;
static hidpp_thread_t g_th_refresh;
static int            g_th_refresh_valid = 0;
static hidpp_thread_t g_th_notify;
static int            g_th_notify_valid = 0;

/* Live & frozen snapshot states (protected by g_snap_lock) */
static hidpp_state_t  g_live_state;
static hidpp_state_t  g_frozen_snap;

/* Saved user target (protected by g_hid_lock) */
static int            g_has_saved = 0;
static int32_t        g_saved_dpi = HIDPP_DEFAULT_DPI;
static int32_t        g_saved_lod = HIDPP_DEFAULT_LOD;

/* Endpoint & feature cache (protected by g_hid_lock) */
static uint64_t              g_endpoint_gen = 1;
static uint64_t              g_last_read_gen = 0;
static int                   g_cache_valid = 0;
static char                  g_cached_endpoint_id[HIDPP_ENDPOINT_ID_MAX];
static hidpp_endpoint_kind_t g_cached_kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
static uint8_t               g_cached_dev_idx = 0x01;
static uint8_t               g_cached_idx_dpi = 0;
static uint8_t               g_cached_idx_ob = 0;
static uint8_t               g_cached_idx_bat = 0;
static uint8_t               g_cached_idx_name = 0;
static int                   g_cached_caps_read = 0;
static int                   g_cached_caps_confirmed = 0;
static int                   g_cached_caps_fails = 0;
static int                   g_cached_fn5_saw_lod = 0;
static int                   g_cached_has_indep_y = 0;
static int                   g_cached_has_lod = 0;
static uint64_t              g_notif_enabled_gen = 0;
static char                  g_last_connected_ep_id[HIDPP_ENDPOINT_ID_MAX];
static uint8_t               g_last_connected_dev_idx = 0;
static int                   g_device_switched = 0;

static void init_default_state(hidpp_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->rev = 0;
    s->applied_seq = 0;
    s->connected = 0;
    s->perm_denied = 0;
    s->dpi_x = HIDPP_DEFAULT_DPI;
    s->dpi_y = HIDPP_DEFAULT_DPI;
    s->lod = HIDPP_DEFAULT_LOD;
    s->battery = -1;
    s->mode = -1;
    snprintf(s->device_name, sizeof(s->device_name), "%s", HIDPP_DEFAULT_DEVICE_NAME);
}

static void core_init_primitives(void) {
    hidpp_mutex_init(&g_life_lock);
    hidpp_cond_init(&g_stop_cv);
    g_stop_claimed = 0;
    g_stop_done = 0;
    hidpp_mutex_init(&g_hid_lock);
    hidpp_mutex_init(&g_snap_lock);
    hidpp_cond_init(&g_rev_cv);
    hidpp_mutex_init(&g_wake_lock);
    hidpp_cond_init(&g_wake_cv);
    hidpp_mutex_init(&g_refresh_lock);
    hidpp_cond_init(&g_refresh_cv);
    init_default_state(&g_live_state);
    init_default_state(&g_frozen_snap);
    g_stopping = 0;
}

#if defined(_WIN32)
static INIT_ONCE g_core_init_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK core_init_once_win(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    core_init_primitives();
    return TRUE;
}
static void ensure_core_mutexes(void) {
    InitOnceExecuteOnce(&g_core_init_once, core_init_once_win, NULL, NULL);
}
#else
static pthread_once_t g_core_init_once = PTHREAD_ONCE_INIT;
static void ensure_core_mutexes(void) {
    pthread_once(&g_core_init_once, core_init_primitives);
}
#endif

int hidpp_core_is_stopping(void) {
    ensure_core_mutexes();
    hidpp_mutex_lock(&g_snap_lock);
    int s = g_stopping;
    hidpp_mutex_unlock(&g_snap_lock);
    return s;
}

static void ensure_core_init(void) {
    ensure_core_mutexes();
    if (!hidpp_core_is_stopping()) {
        hidpp_plat_init();
    }
}

static void commit_state_seq(const hidpp_state_t *next, int bump_rev, int32_t min_applied_seq) {
    hidpp_mutex_lock(&g_snap_lock);
    int32_t rev = g_live_state.rev;
    int32_t prev_seq = g_live_state.applied_seq;
    g_live_state = *next;
    int32_t new_seq = g_live_state.applied_seq;
    if (prev_seq > new_seq) new_seq = prev_seq;
    if (min_applied_seq > new_seq) new_seq = min_applied_seq;
    g_live_state.applied_seq = new_seq;
    g_live_state.rev = bump_rev ? (rev + 1) : rev;
    if (bump_rev) {
        hidpp_cond_broadcast(&g_rev_cv);
    }
    hidpp_mutex_unlock(&g_snap_lock);
}

static void commit_state(const hidpp_state_t *next, int bump_rev) {
    commit_state_seq(next, bump_rev, 0);
}

static void bump_state_rev_only(void) {
    hidpp_mutex_lock(&g_snap_lock);
    g_live_state.rev++;
    hidpp_cond_broadcast(&g_rev_cv);
    hidpp_mutex_unlock(&g_snap_lock);
}

static hidpp_state_t load_live_state(void) {
    hidpp_mutex_lock(&g_snap_lock);
    hidpp_state_t s = g_live_state;
    hidpp_mutex_unlock(&g_snap_lock);
    return s;
}

static void invalidate_feature_cache_locked(void) {
    g_cached_idx_dpi = 0;
    g_cached_idx_ob = 0;
    g_cached_idx_bat = 0;
    g_cached_idx_name = 0;
    g_cached_caps_read = 0;
    g_cached_caps_confirmed = 0;
    g_cached_caps_fails = 0;
    g_cached_fn5_saw_lod = 0;
    g_cached_has_indep_y = 0;
    g_cached_has_lod = 0;
}

static void invalidate_cache_locked(void) {
    g_cache_valid = 0;
    g_endpoint_gen++;
    g_cached_endpoint_id[0] = '\0';
    invalidate_feature_cache_locked();
}

static void consume_dirty_flags_locked(void) {
    hidpp_mutex_lock(&g_snap_lock);
    int topo = g_topology_dirty;
    int feat = g_feat_cache_dirty;
    g_topology_dirty = 0;
    g_feat_cache_dirty = 0;
    hidpp_mutex_unlock(&g_snap_lock);

    if (topo) {
        invalidate_cache_locked();
    } else if (feat) {
        invalidate_feature_cache_locked();
    }
}

/* ---- Strict HID++ 2.0 transaction engine ---- */

enum {
    HIDPP_XFER_OK          =  0,
    HIDPP_XFER_ERR_IO      = -1,
    HIDPP_XFER_ERR_TIMEOUT = -2,
    HIDPP_XFER_ERR_OFFLINE = -3,
    HIDPP_XFER_ERR_HIDPP2  = -4
};

static int hidpp_xfer(hidpp_dev_t *dev, uint8_t dev_idx, uint8_t feat_idx, uint8_t fn,
                      const uint8_t *payload, int payload_len,
                      uint8_t resp[HIDPP_LONG_LEN], int *out_got) {
    uint8_t fn_sw = (uint8_t)(((fn & 0x0Fu) << 4) | (HIDPP_SWID & 0x0Fu));
    uint8_t req[HIDPP_LONG_LEN];
    memset(req, 0, sizeof(req));
    req[0] = HIDPP_LONG_REPORT_ID;
    req[1] = dev_idx;
    req[2] = feat_idx;
    req[3] = fn_sw;
    if (payload && payload_len > 0) {
        int n = payload_len > 16 ? 16 : payload_len;
        memcpy(req + 4, payload, (size_t)n);
    }

    hidpp_plat_drain(dev);

    int w = hidpp_plat_write(dev, req, HIDPP_LONG_LEN);
    if (w < HIDPP_LONG_LEN) return HIDPP_XFER_ERR_IO;

    uint64_t deadline = hidpp_plat_now_ms() + (uint64_t)HIDPP_XFER_TIMEOUT_MS;
    for (;;) {
        uint64_t now = hidpp_plat_now_ms();
        if (now >= deadline) return HIDPP_XFER_ERR_TIMEOUT;
        uint64_t rem64 = deadline - now;
        int rem = (rem64 > (uint64_t)HIDPP_XFER_TIMEOUT_MS) ? HIDPP_XFER_TIMEOUT_MS : (int)rem64;
        if (rem <= 0) rem = 1;

        uint8_t r[64];
        memset(r, 0, sizeof(r));
        int got = hidpp_plat_read(dev, r, sizeof(r), rem);
        if (got < 0) return HIDPP_XFER_ERR_IO;
        if (got == 0) return HIDPP_XFER_ERR_TIMEOUT;

        /* 1. Exact long or short HID++ 2.0 response */
        if (((r[0] == HIDPP_LONG_REPORT_ID && got >= HIDPP_LONG_LEN) ||
             (r[0] == HIDPP_SHORT_REPORT_ID && got >= HIDPP_SHORT_LEN)) &&
            r[1] == dev_idx &&
            r[2] == feat_idx &&
            r[3] == fn_sw) {
            int cpy = got > HIDPP_LONG_LEN ? HIDPP_LONG_LEN : got;
            memset(resp, 0, HIDPP_LONG_LEN);
            memcpy(resp, r, (size_t)cpy);
            if (out_got) *out_got = got;
            return HIDPP_XFER_OK;
        }

        /* 2. Exact HID++ 2.0 protocol error */
        if (got >= HIDPP_SHORT_LEN &&
            (r[0] == HIDPP_SHORT_REPORT_ID || r[0] == HIDPP_LONG_REPORT_ID) &&
            r[1] == dev_idx &&
            r[2] == 0xFFu &&
            r[3] == feat_idx &&
            r[4] == fn_sw) {
            int cpy = got > HIDPP_LONG_LEN ? HIDPP_LONG_LEN : got;
            memset(resp, 0, HIDPP_LONG_LEN);
            memcpy(resp, r, (size_t)cpy);
            if (out_got) *out_got = got;
            return HIDPP_XFER_ERR_HIDPP2;
        }

        /* 3. HID++ 1.0 receiver error (e.g. mouse offline / invalid sub-id): fail fast.
         * Frame layout: [0x10, dev_idx, 0x8F, sub_id(=feat_idx), address(=fn_sw), err_code, 0x00]. */
        if (got >= HIDPP_SHORT_LEN &&
            r[0] == HIDPP_SHORT_REPORT_ID &&
            r[1] == dev_idx &&
            r[2] == 0x8Fu &&
            r[3] == feat_idx &&
            r[4] == fn_sw) {
            int cpy = got > HIDPP_LONG_LEN ? HIDPP_LONG_LEN : got;
            memset(resp, 0, HIDPP_LONG_LEN);
            memcpy(resp, r, (size_t)cpy);
            if (out_got) *out_got = got;
            return HIDPP_XFER_ERR_OFFLINE;
        }

        /* Unrelated notification or stale frame: keep waiting until deadline */
    }
}

static uint8_t get_feature_idx(hidpp_dev_t *dev, uint8_t dev_idx, uint16_t fid) {
    uint8_t p[2] = { (uint8_t)(fid >> 8), (uint8_t)(fid & 0xFFu) };
    uint8_t r[HIDPP_LONG_LEN];
    int got = 0;
    if (hidpp_xfer(dev, dev_idx, 0x00u, 0x00u, p, 2, r, &got) != HIDPP_XFER_OK) {
        return 0;
    }
    return r[4];
}

/* Strict HID++ 1.0 register 0x00 read-modify-write to enable wireless notifications
 * without clobbering existing receiver notification flags.
 * Aborts with zero writes if 0x81 read fails/times out; requires 0x80 0x00 ACK to return 1. */
static int try_enable_wireless_notif(hidpp_dev_t *dev) {
    uint8_t reg[3] = { 0x00u, 0x00u, 0x00u };
    int read_ok = 0;

    uint8_t rreq[HIDPP_SHORT_LEN] = {
        HIDPP_SHORT_REPORT_ID, 0xFFu, 0x81u, 0x00u, 0x00u, 0x00u, 0x00u
    };
    hidpp_plat_drain(dev);
    if (hidpp_plat_write(dev, rreq, sizeof(rreq)) < HIDPP_SHORT_LEN) {
        return 0;
    }

    uint64_t rdeadline = hidpp_plat_now_ms() + 150u;
    for (;;) {
        uint64_t now = hidpp_plat_now_ms();
        if (now >= rdeadline) return 0;
        uint64_t rem64 = rdeadline - now;
        int rem = rem64 > 150u ? 150 : (int)rem64;
        if (rem <= 0) rem = 1;
        uint8_t r[64];
        memset(r, 0, sizeof(r));
        int got = hidpp_plat_read(dev, r, sizeof(r), rem);
        if (got <= 0) return 0;
        if (got >= HIDPP_SHORT_LEN &&
            r[0] == HIDPP_SHORT_REPORT_ID &&
            r[1] == 0xFFu &&
            r[2] == 0x81u &&
            r[3] == 0x00u) {
            reg[0] = r[4];
            reg[1] = (uint8_t)(r[5] | 0x01u);
            reg[2] = r[6];
            read_ok = 1;
            break;
        }
        if (got >= HIDPP_SHORT_LEN &&
            r[0] == HIDPP_SHORT_REPORT_ID &&
            r[1] == 0xFFu &&
            r[2] == 0x8Fu &&
            r[3] == 0x81u &&
            r[4] == 0x00u) {
            return 0;
        }
    }
    if (!read_ok) return 0;

    uint8_t wreq[HIDPP_SHORT_LEN] = {
        HIDPP_SHORT_REPORT_ID, 0xFFu, 0x80u, 0x00u, reg[0], reg[1], reg[2]
    };
    if (hidpp_plat_write(dev, wreq, sizeof(wreq)) < HIDPP_SHORT_LEN) {
        return 0;
    }

    uint64_t wdeadline = hidpp_plat_now_ms() + 150u;
    for (;;) {
        uint64_t now = hidpp_plat_now_ms();
        if (now >= wdeadline) return 0;
        uint64_t rem64 = wdeadline - now;
        int rem = rem64 > 150u ? 150 : (int)rem64;
        if (rem <= 0) rem = 1;
        uint8_t r[64];
        memset(r, 0, sizeof(r));
        int got = hidpp_plat_read(dev, r, sizeof(r), rem);
        if (got <= 0) return 0;
        if (got >= HIDPP_SHORT_LEN &&
            r[0] == HIDPP_SHORT_REPORT_ID &&
            r[1] == 0xFFu &&
            r[2] == 0x80u &&
            r[3] == 0x00u) {
            return 1;
        }
        if (got >= HIDPP_SHORT_LEN &&
            r[0] == HIDPP_SHORT_REPORT_ID &&
            r[1] == 0xFFu &&
            r[2] == 0x8Fu &&
            r[3] == 0x80u &&
            r[4] == 0x00u) {
            return 0;
        }
    }
}

/* Dynamic device name via HID++ 2.0 feature 0x0005 (preserves valid UTF-8, filters control chars). */
static void read_device_name(hidpp_dev_t *dev, uint8_t dev_idx, uint8_t idx_name,
                             char out_name[64]) {
    if (idx_name == 0) return;
    uint8_t r[HIDPP_LONG_LEN];
    int got = 0;
    if (hidpp_xfer(dev, dev_idx, idx_name, 0, NULL, 0, r, &got) != HIDPP_XFER_OK) {
        return;
    }
    int total = (int)r[4];
    if (total <= 0) return;
    if (total > 63) total = 63;

    char buf[64];
    int pos = 0;
    for (int off = 0; off < total; off += 16) {
        uint8_t p[1] = { (uint8_t)off };
        if (hidpp_xfer(dev, dev_idx, idx_name, 1, p, 1, r, &got) != HIDPP_XFER_OK) {
            return;
        }
        int chunk = total - off;
        if (chunk > 16) chunk = 16;
        for (int i = 0; i < chunk && pos < 63; i++) {
            uint8_t ch = r[4 + i];
            if (ch == 0) {
                off = total;
                break;
            }
            if (ch >= 0x20u && ch != 0x7Fu) {
                buf[pos++] = (char)ch;
            }
        }
    }
    while (pos > 0 && buf[pos - 1] == ' ') pos--;
    buf[pos] = '\0';
    if (pos > 0) {
        memcpy(out_name, buf, (size_t)pos + 1);
    }
}

/* Opens cached endpoint or probes all enumerated candidates until 0x2202 is found. */
static hidpp_dev_t *open_active_endpoint_locked(int *out_perm_denied) {
    if (out_perm_denied) *out_perm_denied = 0;

    if (g_cache_valid && g_cached_endpoint_id[0] != '\0') {
        int perm = 0;
        hidpp_dev_t *dev = hidpp_plat_open(g_cached_endpoint_id, 0, &perm);
        if (dev) return dev;
        invalidate_cache_locked();
    }

    hidpp_endpoint_info_t eps[HIDPP_MAX_ENDPOINTS];
    int perm_denied = 0;
    int any_opened = 0;
    int count = hidpp_plat_enumerate(eps, HIDPP_MAX_ENDPOINTS, &perm_denied);

    for (int i = 0; i < count; i++) {
        int ep_perm = 0;
        hidpp_dev_t *dev = hidpp_plat_open(eps[i].id, 0, &ep_perm);
        if (!dev) {
            if (ep_perm && !any_opened) perm_denied = 1;
            continue;
        }
        any_opened = 1;
        perm_denied = 0;

        uint8_t slots[6];
        int nslots = 0;
        if (eps[i].kind == HIDPP_KIND_LIGHTSPEED_RECEIVER) {
            slots[0] = 0x01u;
            nslots = 1;
        } else if (eps[i].kind == HIDPP_KIND_DIRECT_WIRED) {
            slots[0] = 0xFFu;
            slots[1] = 0x00u; /* BLE HID++ 2.0 direct slot */
            nslots = 2;
        } else {
            for (int s = 0; s < 6; s++) slots[s] = (uint8_t)(s + 1);
            nslots = 6;
        }

        for (int s = 0; s < nslots; s++) {
            uint8_t idx_dpi = get_feature_idx(dev, slots[s], HIDPP_FEAT_EXT_DPI);
            if (idx_dpi != 0) {
                g_endpoint_gen++;
                g_cache_valid = 1;
                memcpy(g_cached_endpoint_id, eps[i].id, sizeof(g_cached_endpoint_id));
                g_cached_endpoint_id[sizeof(g_cached_endpoint_id) - 1] = '\0';
                g_cached_kind = eps[i].kind;
                g_cached_dev_idx = slots[s];
                invalidate_feature_cache_locked();
                g_cached_idx_dpi = idx_dpi;

                if (g_last_connected_ep_id[0] == '\0' ||
                    strcmp(g_last_connected_ep_id, eps[i].id) != 0 ||
                    g_last_connected_dev_idx != slots[s]) {
                    g_device_switched = 1;
                    memcpy(g_last_connected_ep_id, eps[i].id, sizeof(g_last_connected_ep_id));
                    g_last_connected_ep_id[sizeof(g_last_connected_ep_id) - 1] = '\0';
                    g_last_connected_dev_idx = slots[s];
                }

                if ((eps[i].kind == HIDPP_KIND_LIGHTSPEED_RECEIVER ||
                     eps[i].kind == HIDPP_KIND_MULTI_RECEIVER) &&
                    g_notif_enabled_gen != g_endpoint_gen) {
                    if (try_enable_wireless_notif(dev)) {
                        g_notif_enabled_gen = g_endpoint_gen;
                    }
                }
                if (out_perm_denied) *out_perm_denied = 0;
                return dev;
            }
        }
        hidpp_plat_close(dev);
    }

    if (out_perm_denied) *out_perm_denied = any_opened ? 0 : perm_denied;
    return NULL;
}

static void ensure_dpi_caps_locked(hidpp_dev_t *dev, uint8_t dev_idx, uint8_t idx_dpi) {
    if (g_cached_caps_read) return;

    uint8_t r[HIDPP_LONG_LEN];
    int got = 0;
    uint8_t p0[1] = { 0x00u };
    /* 0x2202 fn=1 getSensorCapabilities response:
     * [0x11, dev_idx, feat_idx, fn_sw, sensorIdx(r[4]), numDpiLevels(r[5]), capabilities(r[6]), ...] */
    int rc = hidpp_xfer(dev, dev_idx, idx_dpi, 1, p0, 1, r, &got);
    if (rc == HIDPP_XFER_OK && got >= 7 && r[4] == 0x00u) {
        uint8_t flags = r[6];
        g_cached_has_indep_y    = (flags & 0x01u) ? 1 : 0;
        g_cached_has_lod        = (flags & 0x02u) ? 1 : 0;
        g_cached_caps_confirmed = 1;
        g_cached_caps_read      = 1;
        g_cached_caps_fails     = 0;
    } else {
        /* Safe conservative fallback: never send unverified Y/LOD when fn=1 fails.
         * Bound retries on explicit HID++ 2.0 error or repeated timeouts so refreshes don't stall. */
        g_cached_has_indep_y = 0;
        g_cached_has_lod     = g_cached_fn5_saw_lod;
        g_cached_caps_fails++;
        if (rc == HIDPP_XFER_ERR_HIDPP2 || g_cached_caps_fails >= 2) {
            g_cached_caps_read = 1;
        }
    }
}

static int read_sensor_dpi_locked(hidpp_dev_t *dev, uint8_t dev_idx, uint8_t idx_dpi,
                                  int32_t *out_x, int32_t *out_y, int32_t *out_lod,
                                  int32_t fallback_lod) {
    uint8_t r[HIDPP_LONG_LEN];
    int got = 0;
    uint8_t p0[1] = { 0x00u };
    if (hidpp_xfer(dev, dev_idx, idx_dpi, 5, p0, 1, r, &got) != HIDPP_XFER_OK || got < 14) {
        return 0;
    }
    int32_t dx = ((int32_t)r[5] << 8) | r[6];
    if (dx == 0) {
        dx = ((int32_t)r[7] << 8) | r[8]; /* default_dpi_x fallback */
    }
    if (dx < HIDPP_MIN_DPI || dx > HIDPP_MAX_DPI) {
        return 0;
    }
    int32_t dy = ((int32_t)r[9] << 8) | r[10];
    if (dy < HIDPP_MIN_DPI) dy = dx;
    if (r[13] >= 1 && r[13] <= 3) {
        g_cached_fn5_saw_lod = 1;
        if (!g_cached_caps_confirmed) g_cached_has_lod = 1;
    } else if (r[13] == 0) {
        g_cached_fn5_saw_lod = 0;
        g_cached_has_lod = 0;
    }
    int32_t lod = (r[13] >= 1 && r[13] <= 3) ? (int32_t)r[13] : fallback_lod;
    if (out_x) *out_x = dx;
    if (out_y) *out_y = dy;
    if (out_lod) *out_lod = lod;
    return 1;
}

static int32_t read_state_locked(int bump_rev, int32_t min_applied_seq) {
    hidpp_state_t st = load_live_state();
    int perm_denied = 0;

    for (int pass = 0; pass < 2; pass++) {
        int was_cached = g_cache_valid;
        hidpp_dev_t *dev = open_active_endpoint_locked(&perm_denied);
        if (!dev) {
            st.connected = 0;
            st.perm_denied = perm_denied ? 1 : 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return 0;
        }

        uint8_t dev_idx = g_cached_dev_idx;
        uint8_t idx_dpi = g_cached_idx_dpi;
        if (idx_dpi == 0) {
            idx_dpi = get_feature_idx(dev, dev_idx, HIDPP_FEAT_EXT_DPI);
            g_cached_idx_dpi = idx_dpi;
        }
        if (idx_dpi == 0) {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            if (was_cached) continue;
            st.connected = 0;
            st.perm_denied = perm_denied ? 1 : 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return 0;
        }

        ensure_dpi_caps_locked(dev, dev_idx, idx_dpi);

        int32_t dx = 0, dy = 0, lod = st.lod;
        if (read_sensor_dpi_locked(dev, dev_idx, idx_dpi, &dx, &dy, &lod, st.lod)) {
            st.dpi_x = dx;
            st.dpi_y = dy;
            st.lod = lod;
            st.connected = 1;
            st.perm_denied = 0;
        } else {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            if (was_cached) continue;
            st.connected = 0;
            st.perm_denied = perm_denied ? 1 : 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return 0;
        }

        if (g_device_switched) {
            st.battery = -1;
            st.mode = -1;
            st.device_name[0] = '\0';
        }

        uint8_t r[HIDPP_LONG_LEN];
        int got = 0;
        if (g_cached_idx_bat == 0) {
            g_cached_idx_bat = get_feature_idx(dev, dev_idx, HIDPP_FEAT_BATTERY);
        }
        if (g_cached_idx_bat != 0 &&
            hidpp_xfer(dev, dev_idx, g_cached_idx_bat, 1, NULL, 0, r, &got) == HIDPP_XFER_OK &&
            got >= 5) {
            st.battery = r[4];
        }

        if (g_cached_idx_ob == 0) {
            g_cached_idx_ob = get_feature_idx(dev, dev_idx, HIDPP_FEAT_ONBOARD);
        }
        if (g_cached_idx_ob != 0 &&
            hidpp_xfer(dev, dev_idx, g_cached_idx_ob, 2, NULL, 0, r, &got) == HIDPP_XFER_OK &&
            got >= 5) {
            st.mode = r[4];
        }

        if (g_cached_idx_name == 0) {
            g_cached_idx_name = get_feature_idx(dev, dev_idx, HIDPP_FEAT_DEV_NAME);
        }
        if (g_cached_idx_name != 0) {
            read_device_name(dev, dev_idx, g_cached_idx_name, st.device_name);
        }
        if (st.device_name[0] == '\0') {
            snprintf(st.device_name, sizeof(st.device_name), "%s", HIDPP_DEFAULT_DEVICE_NAME);
        }
        g_device_switched = 0;
        g_last_read_gen = g_endpoint_gen;

        hidpp_plat_close(dev);
        commit_state_seq(&st, bump_rev, min_applied_seq);
        return 1;
    }

    st.connected = 0;
    st.perm_denied = perm_denied ? 1 : 0;
    commit_state_seq(&st, bump_rev, min_applied_seq);
    return 0;
}

static int32_t set_dpi_locked(int32_t x, int32_t y, int32_t lod, int save_cfg,
                              int bump_rev, int32_t min_applied_seq) {
    if (x < HIDPP_MIN_DPI) x = HIDPP_MIN_DPI;
    if (x > HIDPP_MAX_DPI) x = HIDPP_MAX_DPI;
    if (y < HIDPP_MIN_DPI) y = HIDPP_MIN_DPI;
    if (y > HIDPP_MAX_DPI) y = HIDPP_MAX_DPI;
    if (lod < 1) lod = 1;
    if (lod > 3) lod = 3;

    hidpp_state_t st = load_live_state();
    int perm_denied = 0;

    for (int pass = 0; pass < 2; pass++) {
        int was_cached = g_cache_valid;
        hidpp_dev_t *dev = open_active_endpoint_locked(&perm_denied);
        if (!dev) {
            st.connected = 0;
            st.perm_denied = perm_denied ? 1 : 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return -1;
        }

        uint8_t dev_idx = g_cached_dev_idx;
        uint8_t idx_dpi = g_cached_idx_dpi;
        if (idx_dpi == 0) {
            idx_dpi = get_feature_idx(dev, dev_idx, HIDPP_FEAT_EXT_DPI);
            g_cached_idx_dpi = idx_dpi;
        }
        if (idx_dpi == 0) {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            if (was_cached) continue;
            st.connected = 0;
            st.perm_denied = perm_denied ? 1 : 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return -2;
        }

        ensure_dpi_caps_locked(dev, dev_idx, idx_dpi);

        uint8_t r[HIDPP_LONG_LEN];
        int got = 0;
        if (g_cached_idx_ob == 0) {
            g_cached_idx_ob = get_feature_idx(dev, dev_idx, HIDPP_FEAT_ONBOARD);
        }
        if (g_cached_idx_ob != 0) {
            uint8_t pm[1] = { 0x02u };
            if (hidpp_xfer(dev, dev_idx, g_cached_idx_ob, 1, pm, 1, r, &got) == HIDPP_XFER_OK) {
                st.mode = 2;
            }
        }

        int32_t y_send = g_cached_has_indep_y ? y : 0;
        int32_t lod_send = g_cached_has_lod ? lod : 0;
        uint8_t pd[6] = {
            0x00u,
            (uint8_t)((x >> 8) & 0xFF),
            (uint8_t)(x & 0xFF),
            (uint8_t)((y_send >> 8) & 0xFF),
            (uint8_t)(y_send & 0xFF),
            (uint8_t)lod_send
        };
        int rc = hidpp_xfer(dev, dev_idx, idx_dpi, 6, pd, 6, r, &got);

        if (rc == HIDPP_XFER_ERR_HIDPP2) {
            hidpp_plat_close(dev);
            return -(100 + (int32_t)r[5]);
        }
        if (rc != HIDPP_XFER_OK) {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            if (was_cached) continue;
            st.connected = 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return -3;
        }

        /* Always confirm actual applied DPI/LOD from sensor via fn=5 readback.
         * Never retry onto a different re-enumerated mouse after fn=6 already landed on this target. */
        int32_t rx = 0, ry = 0, rlod = st.lod;
        if (!read_sensor_dpi_locked(dev, dev_idx, idx_dpi, &rx, &ry, &rlod, st.lod)) {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            st.connected = 0;
            commit_state_seq(&st, bump_rev, min_applied_seq);
            return -4;
        }
        st.dpi_x = rx;
        st.dpi_y = ry;
        st.lod = rlod;
        hidpp_plat_close(dev);

        st.connected = 1;
        st.perm_denied = 0;

        if (save_cfg) {
            g_has_saved = 1;
            g_saved_dpi = st.dpi_x;
            g_saved_lod = st.lod;
            (void)hidpp_plat_config_write(g_saved_dpi, g_saved_lod);
        }
        commit_state_seq(&st, bump_rev, min_applied_seq);
        return 0;
    }

    return -3;
}

static int ensure_dpi_fast_locked(int force_write, int *out_changed) {
    if (out_changed) *out_changed = 0;
    hidpp_state_t st = load_live_state();
    int perm_denied = 0;

    hidpp_dev_t *dev = open_active_endpoint_locked(&perm_denied);
    if (!dev) {
        if (st.connected != 0 || st.perm_denied != (perm_denied ? 1 : 0)) {
            st.connected = 0;
            st.perm_denied = perm_denied ? 1 : 0;
            commit_state(&st, 1);
            if (out_changed) *out_changed = 1;
        }
        return 0;
    }

    uint8_t dev_idx = g_cached_dev_idx;
    uint8_t idx_dpi = g_cached_idx_dpi;
    if (idx_dpi == 0) {
        idx_dpi = get_feature_idx(dev, dev_idx, HIDPP_FEAT_EXT_DPI);
        g_cached_idx_dpi = idx_dpi;
    }
    if (idx_dpi == 0) {
        hidpp_plat_close(dev);
        invalidate_cache_locked();
        return 0;
    }

    ensure_dpi_caps_locked(dev, dev_idx, idx_dpi);

    uint8_t r[HIDPP_LONG_LEN];
    int got = 0;
    int need_write = force_write;

    if (!need_write) {
        int32_t cx = 0, cy = 0, clod = st.lod;
        if (read_sensor_dpi_locked(dev, dev_idx, idx_dpi, &cx, &cy, &clod, st.lod)) {
            int diff = (st.connected == 0 || st.dpi_x != cx || st.dpi_y != cy || st.lod != clod);
            st.dpi_x = cx;
            st.dpi_y = cy;
            st.lod = clod;
            st.connected = 1;
            st.perm_denied = 0;
            int y_mismatch = g_cached_has_indep_y && (cy != g_saved_dpi);
            int lod_mismatch = g_cached_has_lod && (clod != g_saved_lod);
            if (cx != g_saved_dpi || y_mismatch || lod_mismatch) {
                need_write = 1;
            } else if (diff && !g_device_switched) {
                commit_state(&st, 1);
                if (out_changed) *out_changed = 1;
            }
        } else {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            return 0;
        }
    }

    if (need_write) {
        if (g_cached_idx_ob == 0) {
            g_cached_idx_ob = get_feature_idx(dev, dev_idx, HIDPP_FEAT_ONBOARD);
        }
        if (g_cached_idx_ob != 0) {
            uint8_t pm[1] = { 0x02u };
            if (hidpp_xfer(dev, dev_idx, g_cached_idx_ob, 1, pm, 1, r, &got) == HIDPP_XFER_OK) {
                st.mode = 2;
            }
        }

        int32_t x = g_saved_dpi;
        int32_t y_send = g_cached_has_indep_y ? x : 0;
        int32_t lod_send = g_cached_has_lod ? g_saved_lod : 0;
        uint8_t pd[6] = {
            0x00u,
            (uint8_t)((x >> 8) & 0xFF),
            (uint8_t)(x & 0xFF),
            (uint8_t)((y_send >> 8) & 0xFF),
            (uint8_t)(y_send & 0xFF),
            (uint8_t)lod_send
        };
        if (hidpp_xfer(dev, dev_idx, idx_dpi, 6, pd, 6, r, &got) == HIDPP_XFER_OK) {
            /* Always confirm actual applied DPI/LOD from sensor via fn=5 readback. */
            int32_t rx = 0, ry = 0, rlod = st.lod;
            if (!read_sensor_dpi_locked(dev, dev_idx, idx_dpi, &rx, &ry, &rlod, st.lod)) {
                hidpp_plat_close(dev);
                invalidate_cache_locked();
                return 0;
            }
            st.dpi_x = rx;
            st.dpi_y = ry;
            st.lod = rlod;
            st.connected = 1;
            st.perm_denied = 0;
            /* Sync saved targets with device-confirmed values to prevent repeated 2s writebacks. */
            g_saved_dpi = st.dpi_x;
            g_saved_lod = st.lod;
            if (!g_device_switched) {
                commit_state(&st, 1);
                if (out_changed) *out_changed = 1;
            }
        } else {
            hidpp_plat_close(dev);
            invalidate_cache_locked();
            return 0;
        }
    }

    hidpp_plat_close(dev);
    return 1;
}

/* ---- Background threads ---- */

void hidpp_core_signal_wake(void) {
    ensure_core_mutexes();
    hidpp_mutex_lock(&g_wake_lock);
    g_wake_burst = 1;
    hidpp_cond_broadcast(&g_wake_cv);
    hidpp_mutex_unlock(&g_wake_lock);
}

void hidpp_core_signal_topology_change(void) {
    ensure_core_mutexes();
    hidpp_mutex_lock(&g_snap_lock);
    g_topology_dirty = 1;
    hidpp_mutex_unlock(&g_snap_lock);

    hidpp_mutex_lock(&g_wake_lock);
    g_wake_burst = 1;
    hidpp_cond_broadcast(&g_wake_cv);
    hidpp_mutex_unlock(&g_wake_lock);

    hidpp_mutex_lock(&g_refresh_lock);
    g_refresh_pending = 1;
    hidpp_cond_broadcast(&g_refresh_cv);
    hidpp_mutex_unlock(&g_refresh_lock);
}

static void hidpp_core_signal_feat_dirty(void) {
    ensure_core_mutexes();
    hidpp_mutex_lock(&g_snap_lock);
    g_feat_cache_dirty = 1;
    hidpp_mutex_unlock(&g_snap_lock);
}

void hidpp_core_signal_wake_reconnect(void) {
    hidpp_core_signal_feat_dirty();
    hidpp_core_signal_wake();
}

static void wake_watcher_main(void *unused) {
    (void)unused;
    static const uint32_t retry_delays[5] = { 40u, 120u, 300u, 1500u, 3500u };

    for (;;) {
        hidpp_mutex_lock(&g_wake_lock);
        if (!g_wake_burst && !hidpp_core_is_stopping()) {
            (void)hidpp_cond_timedwait_ms(&g_wake_cv, &g_wake_lock, 2000u);
        }
        int burst = g_wake_burst;
        g_wake_burst = 0;
        hidpp_mutex_unlock(&g_wake_lock);

        if (hidpp_core_is_stopping()) break;

        hidpp_mutex_lock(&g_snap_lock);
        int dirty = (g_topology_dirty || g_feat_cache_dirty) ? 1 : 0;
        hidpp_mutex_unlock(&g_snap_lock);

        hidpp_mutex_lock(&g_hid_lock);
        consume_dirty_flags_locked();
        int has_saved = g_has_saved;
        if (!has_saved) {
            if (burst || dirty) {
                (void)read_state_locked(1, 0);
            }
            hidpp_mutex_unlock(&g_hid_lock);
            continue;
        }
        hidpp_mutex_unlock(&g_hid_lock);

        if (!burst && !dirty) {
            uint64_t idle_ms = hidpp_plat_mouse_idle_ms();
            if (idle_ms == UINT64_MAX || idle_ms > 15000u) {
                continue;
            }
            hidpp_mutex_lock(&g_hid_lock);
            consume_dirty_flags_locked();
            int changed = 0;
            (void)ensure_dpi_fast_locked(0, &changed);
            hidpp_mutex_unlock(&g_hid_lock);
            continue;
        }

        int restored = 0;
        int changed = 0;
        for (int attempt = 0; attempt < 6; attempt++) {
            if (hidpp_core_is_stopping()) break;
            hidpp_mutex_lock(&g_hid_lock);
            consume_dirty_flags_locked();
            restored = ensure_dpi_fast_locked(0, &changed);
            if (restored && g_device_switched) {
                (void)read_state_locked(1, 0);
                changed = 1;
            }
            hidpp_mutex_unlock(&g_hid_lock);
            if (restored) {
                /* Bump rev only without overwriting concurrent live state fields. */
                if (!changed) {
                    bump_state_rev_only();
                }
                break;
            }
            if (attempt < 5) {
                /* Interruptible sleep */
                hidpp_mutex_lock(&g_wake_lock);
                if (!hidpp_core_is_stopping()) {
                    (void)hidpp_cond_timedwait_ms(&g_wake_cv, &g_wake_lock, retry_delays[attempt]);
                }
                hidpp_mutex_unlock(&g_wake_lock);
            }
        }
    }
}

static void refresher_main(void *unused) {
    (void)unused;
    for (;;) {
        hidpp_mutex_lock(&g_refresh_lock);
        if (!g_refresh_pending && !g_want_pending && !hidpp_core_is_stopping()) {
            (void)hidpp_cond_timedwait_ms(&g_refresh_cv, &g_refresh_lock, 30000u);
        }
        int pending_set = g_want_pending;
        int32_t dpi = g_want_dpi;
        int32_t lod = g_want_lod;
        int32_t req_seq = g_want_seq;
        g_want_pending = 0;
        g_refresh_pending = 0;
        hidpp_mutex_unlock(&g_refresh_lock);

        if (hidpp_core_is_stopping()) break;

        hidpp_mutex_lock(&g_hid_lock);
        consume_dirty_flags_locked();
        if (pending_set) {
            if (set_dpi_locked(dpi, dpi, lod, 1, 1, req_seq) != 0) {
                (void)read_state_locked(1, req_seq);
            }
        } else {
            (void)read_state_locked(1, 0);
        }
        hidpp_mutex_unlock(&g_hid_lock);
    }
}

static void hid_notify_main(void *unused) {
    (void)unused;
    int saw_no_dev = 0;
    int had_disconnect = 0;
    char last_ep_id[HIDPP_ENDPOINT_ID_MAX];
    last_ep_id[0] = '\0';

    while (!hidpp_core_is_stopping()) {
        hidpp_endpoint_info_t eps[HIDPP_MAX_ENDPOINTS];
        int perm_ignored = 0;
        int count = hidpp_plat_enumerate(eps, HIDPP_MAX_ENDPOINTS, &perm_ignored);

        hidpp_dev_t *dev = NULL;
        const char *opened_id = NULL;
        for (int i = 0; i < count; i++) {
            if (eps[i].kind == HIDPP_KIND_LIGHTSPEED_RECEIVER ||
                eps[i].kind == HIDPP_KIND_MULTI_RECEIVER) {
                dev = hidpp_plat_open(eps[i].id, 1, &perm_ignored);
                if (dev) {
                    opened_id = eps[i].id;
                    break;
                }
            }
        }
        if (!dev && count > 0) {
            dev = hidpp_plat_open(eps[0].id, 1, &perm_ignored);
            if (dev) opened_id = eps[0].id;
        }
        if (!dev) {
            saw_no_dev = 1;
            hidpp_plat_sleep_ms(1000u);
            continue;
        }

        if (saw_no_dev || had_disconnect ||
            (opened_id && last_ep_id[0] != '\0' && strcmp(last_ep_id, opened_id) != 0)) {
            hidpp_core_signal_topology_change();
        }
        saw_no_dev = 0;
        had_disconnect = 0;
        if (opened_id) {
            size_t n = strlen(opened_id);
            if (n >= sizeof(last_ep_id)) n = sizeof(last_ep_id) - 1;
            memcpy(last_ep_id, opened_id, n);
            last_ep_id[n] = '\0';
        }

        while (!hidpp_core_is_stopping()) {
            uint8_t buf[64];
            memset(buf, 0, sizeof(buf));
            int got = hidpp_plat_read(dev, buf, sizeof(buf), 500);
            if (got < 0) {
                had_disconnect = 1;
                hidpp_core_signal_topology_change();
                break;
            }
            if (got == 0) continue;
            if (got >= HIDPP_SHORT_LEN &&
                (buf[0] == HIDPP_SHORT_REPORT_ID || buf[0] == HIDPP_LONG_REPORT_ID) &&
                buf[2] == 0x41u &&
                ((buf[4] & 0x40u) == 0u)) {
                hidpp_core_signal_wake_reconnect();
            }
        }

        hidpp_plat_close(dev);
        if (!hidpp_core_is_stopping()) {
            hidpp_plat_sleep_ms(500u);
        }
    }
}

static void start_watchers_once(void) {
    ensure_core_mutexes();
    hidpp_mutex_lock(&g_life_lock);
    hidpp_mutex_lock(&g_snap_lock);
    int stop = g_stopping;
    hidpp_mutex_unlock(&g_snap_lock);

    if (g_watchers_started || stop) {
        hidpp_mutex_unlock(&g_life_lock);
        return;
    }
    g_watchers_started = 1;
    hidpp_plat_start_watchers();
    g_th_wake_valid = hidpp_thread_start(&g_th_wake, wake_watcher_main, NULL);
    g_th_refresh_valid = hidpp_thread_start(&g_th_refresh, refresher_main, NULL);
    if (hidpp_plat_needs_notify_thread()) {
        g_th_notify_valid = hidpp_thread_start(&g_th_notify, hid_notify_main, NULL);
    }
    hidpp_mutex_unlock(&g_life_lock);
}

/* ---- Exported C ABI (28 symbols) ---- */

int32_t mousedpi_started_hidden(void) {
    ensure_core_init();
    return hidpp_plat_started_hidden();
}

int32_t mousedpi_can_hide_to_tray(void) {
    ensure_core_init();
    return hidpp_plat_can_hide_to_tray();
}

int32_t mousedpi_wait_tray_ready(int32_t timeout_ms) {
    ensure_core_init();
    return hidpp_plat_wait_tray_ready(timeout_ms);
}

int32_t mousedpi_peek_config(int32_t which) {
    ensure_core_init();
    int32_t dpi = HIDPP_DEFAULT_DPI;
    int32_t lod = HIDPP_DEFAULT_LOD;
    if (!hidpp_plat_config_read(&dpi, &lod)) {
        dpi = HIDPP_DEFAULT_DPI;
        lod = HIDPP_DEFAULT_LOD;
    }
    if (dpi < HIDPP_MIN_DPI || dpi > HIDPP_MAX_DPI) dpi = HIDPP_DEFAULT_DPI;
    if (lod < 1 || lod > 3) lod = HIDPP_DEFAULT_LOD;
    return (which == 0) ? dpi : lod;
}

int32_t mousedpi_init_and_restore(void) {
    ensure_core_init();
    int32_t cfg_dpi = 0;
    int32_t cfg_lod = HIDPP_DEFAULT_LOD;
    int has_cfg = hidpp_plat_config_read(&cfg_dpi, &cfg_lod);
    if (cfg_lod < 1 || cfg_lod > 3) cfg_lod = HIDPP_DEFAULT_LOD;

    hidpp_mutex_lock(&g_hid_lock);
    int32_t ok = read_state_locked(0, 0);
    if (has_cfg && cfg_dpi >= HIDPP_MIN_DPI && cfg_dpi <= HIDPP_MAX_DPI) {
        g_has_saved = 1;
        g_saved_dpi = cfg_dpi;
        g_saved_lod = cfg_lod;
        hidpp_state_t st = load_live_state();
        int y_mismatch = g_cached_has_indep_y && (st.dpi_y != cfg_dpi);
        int lod_mismatch = g_cached_has_lod && (st.lod != cfg_lod);
        if (ok && (st.dpi_x != cfg_dpi || y_mismatch || lod_mismatch)) {
            if (set_dpi_locked(cfg_dpi, cfg_dpi, cfg_lod, 0, 0, 0) == 0) {
                hidpp_state_t confirmed = load_live_state();
                g_saved_dpi = confirmed.dpi_x;
                g_saved_lod = confirmed.lod;
            }
        } else if (ok && !g_cached_has_lod) {
            g_saved_lod = st.lod;
        } else if (!ok) {
            st.dpi_x = cfg_dpi;
            st.dpi_y = cfg_dpi;
            st.lod = cfg_lod;
            commit_state(&st, 0);
        }
    } else if (ok) {
        hidpp_state_t st = load_live_state();
        g_has_saved = 1;
        g_saved_dpi = st.dpi_x;
        g_saved_lod = st.lod;
        (void)hidpp_plat_config_write(g_saved_dpi, g_saved_lod);
    }
    hidpp_mutex_unlock(&g_hid_lock);

    start_watchers_once();
    return ok;
}

void mousedpi_request_refresh(void) {
    ensure_core_init();
    start_watchers_once();
    hidpp_mutex_lock(&g_refresh_lock);
    g_refresh_pending = 1;
    hidpp_cond_broadcast(&g_refresh_cv);
    hidpp_mutex_unlock(&g_refresh_lock);
}

static void request_set_internal(int32_t seq, int has_seq, int32_t dpi, int32_t lod) {
    ensure_core_init();
    start_watchers_once();
    hidpp_mutex_lock(&g_refresh_lock);
    g_want_pending = 1;
    g_want_dpi = dpi;
    g_want_lod = lod;
    if (has_seq && seq > g_want_seq) {
        g_want_seq = seq;
    }
    hidpp_cond_broadcast(&g_refresh_cv);
    hidpp_mutex_unlock(&g_refresh_lock);
}

void mousedpi_request_set(int32_t dpi, int32_t lod) {
    request_set_internal(0, 0, dpi, lod);
}

void mousedpi_request_set_seq(int32_t seq, int32_t dpi, int32_t lod) {
    request_set_internal(seq, 1, dpi, lod);
}

void mousedpi_request_wake_restore(void) {
    ensure_core_init();
    start_watchers_once();
    hidpp_core_signal_wake();
}

int32_t mousedpi_state_rev(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t r = g_live_state.rev;
    hidpp_mutex_unlock(&g_snap_lock);
    return r;
}

int32_t mousedpi_wait_rev(int32_t seen) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    while (g_live_state.rev == seen && !g_stopping) {
        hidpp_cond_wait(&g_rev_cv, &g_snap_lock);
    }
    int32_t r = g_stopping ? -1 : g_live_state.rev;
    hidpp_mutex_unlock(&g_snap_lock);
    return r;
}

void mousedpi_stop_waits(void) {
    ensure_core_mutexes();
    hidpp_mutex_lock(&g_life_lock);
    if (g_stop_claimed) {
        while (!g_stop_done) {
            hidpp_cond_wait(&g_stop_cv, &g_life_lock);
        }
        hidpp_mutex_unlock(&g_life_lock);
        return;
    }
    g_stop_claimed = 1;
    g_stop_done = 0;

    hidpp_mutex_lock(&g_snap_lock);
    g_stopping = 1;
    hidpp_cond_broadcast(&g_rev_cv);
    hidpp_mutex_unlock(&g_snap_lock);

    hidpp_mutex_lock(&g_wake_lock);
    hidpp_cond_broadcast(&g_wake_cv);
    hidpp_mutex_unlock(&g_wake_lock);

    hidpp_mutex_lock(&g_refresh_lock);
    hidpp_cond_broadcast(&g_refresh_cv);
    hidpp_mutex_unlock(&g_refresh_lock);

    int join_wake = g_th_wake_valid;
    hidpp_thread_t th_wake = g_th_wake;
    g_th_wake_valid = 0;

    int join_refresh = g_th_refresh_valid;
    hidpp_thread_t th_refresh = g_th_refresh;
    g_th_refresh_valid = 0;

    int join_notify = g_th_notify_valid;
    hidpp_thread_t th_notify = g_th_notify;
    g_th_notify_valid = 0;

    g_watchers_started = 0;
    hidpp_mutex_unlock(&g_life_lock);

    hidpp_plat_stop_watchers();

    if (join_wake) hidpp_thread_join(th_wake);
    if (join_refresh) hidpp_thread_join(th_refresh);
    if (join_notify) hidpp_thread_join(th_notify);

    hidpp_mutex_lock(&g_life_lock);
    g_stop_done = 1;
    hidpp_cond_broadcast(&g_stop_cv);
    hidpp_mutex_unlock(&g_life_lock);
}

int32_t mousedpi_get_connected(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_live_state.connected;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_get_dpi_x(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_live_state.dpi_x;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_get_lod(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_live_state.lod;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_get_battery(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_live_state.battery;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_get_autostart(void) {
    ensure_core_init();
    return hidpp_plat_get_autostart();
}

int32_t mousedpi_set_autostart(int32_t enable) {
    ensure_core_init();
    return hidpp_plat_set_autostart(enable);
}

void mousedpi_snapshot_capture(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    g_frozen_snap = g_live_state;
    hidpp_mutex_unlock(&g_snap_lock);
}

int32_t mousedpi_snap_rev(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.rev;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_snap_applied_seq(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.applied_seq;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_snap_connected(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.connected;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_snap_dpi_x(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.dpi_x;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_snap_lod(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.lod;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_snap_battery(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.battery;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

int32_t mousedpi_snap_perm_denied(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    int32_t v = g_frozen_snap.perm_denied;
    hidpp_mutex_unlock(&g_snap_lock);
    return v;
}

char *mousedpi_snap_device_name_dup(void) {
    ensure_core_init();
    hidpp_mutex_lock(&g_snap_lock);
    size_t n = strlen(g_frozen_snap.device_name);
    char *copy = (char *)malloc(n + 1);
    if (copy) memcpy(copy, g_frozen_snap.device_name, n + 1);
    hidpp_mutex_unlock(&g_snap_lock);
    return copy;
}

char *mousedpi_picture_path_dup(void) {
    ensure_core_init();
    return hidpp_plat_picture_path_dup();
}

#ifdef HIDPP_CORE_TEST
/* Reset helper for isolated unit tests */
static void hidpp_core_test_reset(void) {
    ensure_core_mutexes();
    mousedpi_stop_waits();

    hidpp_mutex_lock(&g_life_lock);
    hidpp_mutex_lock(&g_hid_lock);
    invalidate_cache_locked();
    g_last_read_gen = 0;
    g_notif_enabled_gen = 0;
    g_last_connected_ep_id[0] = '\0';
    g_last_connected_dev_idx = 0;
    g_device_switched = 0;
    g_has_saved = 0;
    g_saved_dpi = HIDPP_DEFAULT_DPI;
    g_saved_lod = HIDPP_DEFAULT_LOD;
    hidpp_mutex_unlock(&g_hid_lock);

    hidpp_mutex_lock(&g_snap_lock);
    init_default_state(&g_live_state);
    init_default_state(&g_frozen_snap);
    g_stopping = 0;
    g_topology_dirty = 0;
    g_feat_cache_dirty = 0;
    hidpp_mutex_unlock(&g_snap_lock);

    hidpp_mutex_lock(&g_wake_lock);
    g_wake_burst = 0;
    hidpp_mutex_unlock(&g_wake_lock);

    hidpp_mutex_lock(&g_refresh_lock);
    g_refresh_pending = 0;
    g_want_pending = 0;
    g_want_seq = 0;
    hidpp_mutex_unlock(&g_refresh_lock);

    g_watchers_started = 0;
    g_stop_claimed = 0;
    g_stop_done = 0;
    hidpp_mutex_unlock(&g_life_lock);
}
#endif
