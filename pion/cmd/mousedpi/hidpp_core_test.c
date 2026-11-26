#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#include <unistd.h>
#endif

/* Include the production core directly so we can test with a mock platform layer. */
#include "hidpp_core.c"

/* ---- Mock platform state ---- */

typedef struct mock_frame {
    uint8_t data[64];
    int     len;
    int     delay_ms;
} mock_frame_t;

typedef struct mock_device {
    char                  id[HIDPP_ENDPOINT_ID_MAX];
    uint16_t              vid;
    uint16_t              pid;
    hidpp_endpoint_kind_t kind;
    int                   perm_denied;
    int                   has_dpi_feature; /* 1 if this device responds to 0x2202 */
    uint8_t               active_slot;     /* dev_idx that has 0x2202 */
    int32_t               dpi_x;
    int32_t               dpi_y;
    int32_t               lod;
    int32_t               battery;
    char                  dev_name[64];
    int                   name_fail;
    int                   no_battery;
    int                   no_onboard;
    int                   truncate_dpi_resp;
    int                   offline_10_8f;
    int                   hidpp2_ff_err;
    int                   fn6_reject_ff;
    int                   fn6_zero_ack;
    int                   fn6_fake_echo;
    int32_t               fn6_fake_x;
    int32_t               fn6_fake_y;
    int32_t               fn6_fake_lod;
    int32_t               hw_clamp_x;
    int32_t               hw_clamp_y;
    int32_t               hw_clamp_lod;
    int32_t               last_fn6_x;
    int32_t               last_fn6_y;
    int32_t               last_fn6_lod;
    int                   caps_present;
    uint8_t               caps_num_levels; /* r[5] distractor */
    uint8_t               caps_flags;      /* r[6]: bit 0 = indep Y, bit 1 = LOD */
    int                   fn5_lod_zero;    /* return r[13] == 0 on fn=5 */
    int                   reject_nonzero_y_lod;
    int                   fn6_writes;
    int                   noise_forever;
    int                   notif_reg_writes;
    uint8_t               reg00_init[3];   /* initial register 0x00 reply bytes [4..6] */
    int                   reg00_readable;
    int                   reg00_read_timeout;
    int                   reg00_write_err;
    uint8_t               reg00_written[3];
    int                   drain_calls;
    int                   notify_disconnect_once;

    /* Pre-queued stale frames to test drain */
    mock_frame_t          stale[8];
    int                   stale_count;

    /* Extra unrelated frames injected before real response */
    mock_frame_t          prefix[8];
    int                   prefix_count;
} mock_device_t;

struct hidpp_dev {
    mock_device_t *m;
    int            notify_only;
    mock_frame_t   rx[32];
    int            rx_head;
    int            rx_tail;
    int            rx_count;
};

static hidpp_mutex_t   g_mock_lock;
static int             g_mock_lock_inited = 0;
static mock_device_t   g_mock_devs[8];
static int             g_mock_dev_count = 0;
static int             g_mock_enum_perm_denied = 0;
static int             g_mock_cfg_has = 0;
static int32_t         g_mock_cfg_dpi = 800;
static int32_t         g_mock_cfg_lod = 2;
static int             g_mock_cfg_writes = 0;
static uint64_t        g_mock_idle_ms = UINT64_MAX;
static int32_t         g_mock_autostart = 0;
static int             g_mock_needs_notify = 0;
static int             g_mock_plat_inits = 0;
static mock_frame_t    g_mock_notify_q[16];
static int             g_mock_notify_head = 0;
static int             g_mock_notify_tail = 0;
static int             g_mock_notify_count = 0;
static int             g_mock_notify_err = 0;

static void mock_lock(void) {
    if (!g_mock_lock_inited) {
        hidpp_mutex_init(&g_mock_lock);
        g_mock_lock_inited = 1;
    }
    hidpp_mutex_lock(&g_mock_lock);
}

static void mock_unlock(void) {
    hidpp_mutex_unlock(&g_mock_lock);
}

static void mock_push_notify(const uint8_t *buf, int len) {
    mock_lock();
    if (g_mock_notify_count < 16) {
        mock_frame_t *f = &g_mock_notify_q[g_mock_notify_tail];
        memcpy(f->data, buf, (size_t)len);
        f->len = len;
        f->delay_ms = 0;
        g_mock_notify_tail = (g_mock_notify_tail + 1) % 16;
        g_mock_notify_count++;
    }
    mock_unlock();
}

static void mock_reset(void) {
    hidpp_core_test_reset();
    mock_lock();
    memset(g_mock_devs, 0, sizeof(g_mock_devs));
    g_mock_dev_count = 0;
    g_mock_enum_perm_denied = 0;
    g_mock_cfg_has = 0;
    g_mock_cfg_dpi = 800;
    g_mock_cfg_lod = 2;
    g_mock_cfg_writes = 0;
    g_mock_idle_ms = UINT64_MAX;
    g_mock_autostart = 0;
    g_mock_needs_notify = 0;
    g_mock_plat_inits = 0;
    g_mock_notify_head = 0;
    g_mock_notify_tail = 0;
    g_mock_notify_count = 0;
    g_mock_notify_err = 0;
    mock_unlock();
}

static void dev_push_rx(hidpp_dev_t *d, const uint8_t *buf, int len) {
    if (d->rx_count >= 32) return;
    mock_frame_t *f = &d->rx[d->rx_tail];
    memcpy(f->data, buf, (size_t)len);
    f->len = len;
    f->delay_ms = 0;
    d->rx_tail = (d->rx_tail + 1) % 32;
    d->rx_count++;
}

/* ---- Mock platform hook implementations ---- */

void hidpp_plat_init(void) {
    mock_lock();
    g_mock_plat_inits++;
    mock_unlock();
}
void hidpp_plat_start_watchers(void) {}
void hidpp_plat_stop_watchers(void) {}
int  hidpp_plat_needs_notify_thread(void) {
    mock_lock();
    int v = g_mock_needs_notify;
    mock_unlock();
    return v;
}

uint64_t hidpp_plat_now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

void hidpp_plat_sleep_ms(uint32_t ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
#endif
}

uint64_t hidpp_plat_mouse_idle_ms(void) {
    mock_lock();
    uint64_t v = g_mock_idle_ms;
    mock_unlock();
    return v;
}

int32_t hidpp_plat_started_hidden(void) { return 0; }
int32_t hidpp_plat_can_hide_to_tray(void) { return 1; }
int32_t hidpp_plat_wait_tray_ready(int32_t timeout_ms) { (void)timeout_ms; return 1; }
int32_t hidpp_plat_get_autostart(void) { return g_mock_autostart; }
int32_t hidpp_plat_set_autostart(int32_t enable) {
    g_mock_autostart = enable ? 1 : 0;
    return g_mock_autostart;
}
char *hidpp_plat_picture_path_dup(void) {
    const char *s = "/tmp/mock_mouse.png";
    char *p = (char *)malloc(strlen(s) + 1);
    if (p) memcpy(p, s, strlen(s) + 1);
    return p;
}

int hidpp_plat_config_read(int32_t *out_dpi, int32_t *out_lod) {
    mock_lock();
    int has = g_mock_cfg_has;
    if (has) {
        if (out_dpi) *out_dpi = g_mock_cfg_dpi;
        if (out_lod) *out_lod = g_mock_cfg_lod;
    }
    mock_unlock();
    return has;
}

int hidpp_plat_config_write(int32_t dpi, int32_t lod) {
    mock_lock();
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = dpi;
    g_mock_cfg_lod = lod;
    g_mock_cfg_writes++;
    mock_unlock();
    return 1;
}

int hidpp_plat_enumerate(hidpp_endpoint_info_t *out, int max_count, int *out_perm_denied) {
    mock_lock();
    if (out_perm_denied) *out_perm_denied = g_mock_enum_perm_denied;
    int n = 0;
    for (int i = 0; i < g_mock_dev_count && n < max_count; i++) {
        hidpp_endpoint_info_t *ep = &out[n++];
        memset(ep, 0, sizeof(*ep));
        snprintf(ep->id, sizeof(ep->id), "%s", g_mock_devs[i].id);
        ep->vid = g_mock_devs[i].vid;
        ep->pid = g_mock_devs[i].pid;
        ep->kind = g_mock_devs[i].kind;
    }
    mock_unlock();
    return n;
}

hidpp_dev_t *hidpp_plat_open(const char *endpoint_id, int notify_only, int *out_perm_denied) {
    if (out_perm_denied) *out_perm_denied = 0;
    mock_lock();
    mock_device_t *found = NULL;
    for (int i = 0; i < g_mock_dev_count; i++) {
        if (strcmp(g_mock_devs[i].id, endpoint_id) == 0) {
            if (g_mock_devs[i].perm_denied) {
                if (out_perm_denied) *out_perm_denied = 1;
                mock_unlock();
                return NULL;
            }
            found = &g_mock_devs[i];
            break;
        }
    }
    if (!found) {
        mock_unlock();
        return NULL;
    }
    hidpp_dev_t *d = (hidpp_dev_t *)calloc(1, sizeof(*d));
    assert(d != NULL);
    d->m = found;
    d->notify_only = notify_only;
    for (int i = 0; i < found->stale_count; i++) {
        dev_push_rx(d, found->stale[i].data, found->stale[i].len);
    }
    mock_unlock();
    return d;
}

void hidpp_plat_close(hidpp_dev_t *dev) {
    free(dev);
}

void hidpp_plat_drain(hidpp_dev_t *dev) {
    if (!dev) return;
    mock_lock();
    dev->m->drain_calls++;
    dev->rx_head = 0;
    dev->rx_tail = 0;
    dev->rx_count = 0;
    mock_unlock();
}

int hidpp_plat_write(hidpp_dev_t *dev, const uint8_t *buf, size_t len) {
    if (!dev || !buf || len == 0) return -1;
    mock_lock();
    mock_device_t *m = dev->m;

    /* HID++ 1.0 wireless notification register 0x00 read (0x81) */
    if (len == HIDPP_SHORT_LEN && buf[0] == 0x10 && buf[1] == 0xFF && buf[2] == 0x81 && buf[3] == 0x00) {
        if (m->reg00_read_timeout) {
            /* Drop reply to simulate timeout */
        } else if (m->reg00_readable) {
            uint8_t rep[7] = { 0x10, 0xFF, 0x81, 0x00, m->reg00_init[0], m->reg00_init[1], m->reg00_init[2] };
            dev_push_rx(dev, rep, 7);
        } else {
            uint8_t err[7] = { 0x10, 0xFF, 0x8F, 0x81, 0x00, 0x01, 0x00 };
            dev_push_rx(dev, err, 7);
        }
        mock_unlock();
        return (int)len;
    }

    /* HID++ 1.0 wireless notification register 0x00 write (0x80) */
    if (len == HIDPP_SHORT_LEN && buf[0] == 0x10 && buf[1] == 0xFF && buf[2] == 0x80 && buf[3] == 0x00) {
        m->notif_reg_writes++;
        m->reg00_written[0] = buf[4];
        m->reg00_written[1] = buf[5];
        m->reg00_written[2] = buf[6];
        if (m->reg00_write_err) {
            uint8_t err[7] = { 0x10, 0xFF, 0x8F, 0x80, 0x00, 0x01, 0x00 };
            dev_push_rx(dev, err, 7);
        } else {
            uint8_t ack[7] = { 0x10, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00 };
            dev_push_rx(dev, ack, 7);
        }
        mock_unlock();
        return (int)len;
    }

    if (len < HIDPP_LONG_LEN || buf[0] != 0x11) {
        mock_unlock();
        return -1;
    }

    uint8_t dev_idx  = buf[1];
    uint8_t feat_idx = buf[2];
    uint8_t fn_sw    = buf[3];
    uint8_t fn       = (uint8_t)(fn_sw >> 4);

    if (m->noise_forever) {
        /* Never enqueue a matching response */
        mock_unlock();
        return (int)len;
    }

    /* Inject configured prefix noise frames before real reply */
    for (int i = 0; i < m->prefix_count; i++) {
        dev_push_rx(dev, m->prefix[i].data, m->prefix[i].len);
    }

    if (m->offline_10_8f) {
        uint8_t err[7] = { 0x10, dev_idx, 0x8F, feat_idx, fn_sw, 0x08, 0x00 };
        dev_push_rx(dev, err, 7);
        mock_unlock();
        return (int)len;
    }

    if (m->hidpp2_ff_err) {
        uint8_t err[20] = { 0 };
        err[0] = 0x11;
        err[1] = dev_idx;
        err[2] = 0xFF;
        err[3] = feat_idx;
        err[4] = fn_sw;
        err[5] = 0x02; /* Invalid argument */
        dev_push_rx(dev, err, 20);
        mock_unlock();
        return (int)len;
    }

    if (dev_idx != m->active_slot || !m->has_dpi_feature) {
        /* Offline / unsupported slot responds with HID++ 1.0 0x8F error */
        uint8_t err[7] = { 0x10, dev_idx, 0x8F, feat_idx, fn_sw, 0x08, 0x00 };
        dev_push_rx(dev, err, 7);
        mock_unlock();
        return (int)len;
    }

    uint8_t r[20];
    memset(r, 0, sizeof(r));
    r[0] = 0x11;
    r[1] = dev_idx;
    r[2] = feat_idx;
    r[3] = fn_sw;

    if (feat_idx == 0x00 && fn == 0) {
        /* IRoot getFeature */
        uint16_t fid = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
        if (fid == 0x2202) r[4] = 0x05;
        else if (fid == 0x1004) r[4] = m->no_battery ? 0x00 : 0x06;
        else if (fid == 0x8100) r[4] = m->no_onboard ? 0x00 : 0x07;
        else if (fid == 0x0005) r[4] = 0x08;
        else r[4] = 0x00;
        dev_push_rx(dev, r, 20);
    } else if (feat_idx == 0x05 && fn == 1) {
        /* 0x2202 getSensorCapabilities: r[4]=sensorIdx, r[5]=numDpiLevels, r[6]=capabilities */
        if (m->caps_present) {
            r[4] = 0x00;
            r[5] = m->caps_num_levels;
            r[6] = m->caps_flags;
            dev_push_rx(dev, r, 20);
        } else {
            r[2] = 0xFF;
            r[3] = feat_idx;
            r[4] = fn_sw;
            r[5] = 0x05;
            dev_push_rx(dev, r, 20);
        }
    } else if (feat_idx == 0x05 && fn == 5) {
        /* 0x2202 getSensorDpi */
        r[5]  = (uint8_t)((m->dpi_x >> 8) & 0xFF);
        r[6]  = (uint8_t)(m->dpi_x & 0xFF);
        r[9]  = (uint8_t)((m->dpi_y >> 8) & 0xFF);
        r[10] = (uint8_t)(m->dpi_y & 0xFF);
        r[13] = m->fn5_lod_zero ? 0x00 : (uint8_t)m->lod;
        dev_push_rx(dev, r, m->truncate_dpi_resp ? 10 : 20);
    } else if (feat_idx == 0x05 && fn == 6) {
        /* 0x2202 setSensorDpi */
        m->fn6_writes++;
        int32_t req_x = ((int32_t)buf[5] << 8) | buf[6];
        int32_t req_y = ((int32_t)buf[7] << 8) | buf[8];
        int32_t req_l = buf[9];
        m->last_fn6_x = req_x;
        m->last_fn6_y = req_y;
        m->last_fn6_lod = req_l;
        if (m->fn6_reject_ff || (m->reject_nonzero_y_lod && (req_y != 0 || req_l != 0))) {
            r[2] = 0xFF;
            r[3] = feat_idx;
            r[4] = fn_sw;
            r[5] = 0x02;
            dev_push_rx(dev, r, 20);
        } else if (m->fn6_fake_echo) {
            m->dpi_x = m->hw_clamp_x;
            m->dpi_y = m->hw_clamp_y;
            m->lod   = m->hw_clamp_lod;
            r[5] = (uint8_t)((m->fn6_fake_x >> 8) & 0xFF);
            r[6] = (uint8_t)(m->fn6_fake_x & 0xFF);
            r[7] = (uint8_t)((m->fn6_fake_y >> 8) & 0xFF);
            r[8] = (uint8_t)(m->fn6_fake_y & 0xFF);
            r[9] = (uint8_t)m->fn6_fake_lod;
            dev_push_rx(dev, r, 20);
        } else {
            m->dpi_x = req_x;
            m->dpi_y = (req_y >= HIDPP_MIN_DPI) ? req_y : req_x;
            if (req_l >= 1 && req_l <= 3) m->lod = req_l;
            if (!m->fn6_zero_ack) {
                r[5] = (uint8_t)((m->dpi_x >> 8) & 0xFF);
                r[6] = (uint8_t)(m->dpi_x & 0xFF);
                r[7] = (uint8_t)((m->dpi_y >> 8) & 0xFF);
                r[8] = (uint8_t)(m->dpi_y & 0xFF);
                r[9] = m->fn5_lod_zero ? 0x00 : (uint8_t)m->lod;
            }
            dev_push_rx(dev, r, 20);
        }
    } else if (feat_idx == 0x06 && fn == 1) {
        /* 0x1004 battery */
        r[4] = (uint8_t)m->battery;
        dev_push_rx(dev, r, 20);
    } else if (feat_idx == 0x07 && fn == 2) {
        /* 0x8100 getMode */
        r[4] = 0x02;
        dev_push_rx(dev, r, 20);
    } else if (feat_idx == 0x07 && fn == 1) {
        /* 0x8100 setMode */
        r[4] = buf[4];
        dev_push_rx(dev, r, 20);
    } else if (feat_idx == 0x08 && fn == 0) {
        /* 0x0005 getCount */
        if (m->name_fail) {
            r[2] = 0xFF;
            r[3] = feat_idx;
            r[4] = fn_sw;
            r[5] = 0x05;
        } else {
            r[4] = (uint8_t)strlen(m->dev_name);
        }
        dev_push_rx(dev, r, 20);
    } else if (feat_idx == 0x08 && fn == 1) {
        /* 0x0005 getName(offset) */
        size_t off = buf[4];
        size_t nlen = strlen(m->dev_name);
        for (size_t i = 0; i < 16 && off + i < nlen; i++) {
            r[4 + i] = (uint8_t)m->dev_name[off + i];
        }
        dev_push_rx(dev, r, 20);
    }

    mock_unlock();
    return (int)len;
}

int hidpp_plat_read(hidpp_dev_t *dev, uint8_t *buf, size_t cap, int timeout_ms) {
    if (!dev || !buf || cap == 0) return -1;
    mock_lock();
    if (dev->notify_only) {
        if (g_mock_notify_err) {
            g_mock_notify_err = 0;
            mock_unlock();
            return -1;
        }
        if (g_mock_notify_count > 0) {
            mock_frame_t f = g_mock_notify_q[g_mock_notify_head];
            g_mock_notify_head = (g_mock_notify_head + 1) % 16;
            g_mock_notify_count--;
            mock_unlock();
            int cpy = f.len > (int)cap ? (int)cap : f.len;
            memcpy(buf, f.data, (size_t)cpy);
            return cpy;
        }
        mock_unlock();
        if (timeout_ms > 0) hidpp_plat_sleep_ms((uint32_t)(timeout_ms > 10 ? 10 : timeout_ms));
        return 0;
    }
    if (dev->m->noise_forever) {
        mock_unlock();
        /* Return an unrelated 0x41 notification after 20ms to test single deadline */
        int sleep_for = timeout_ms < 20 ? timeout_ms : 20;
        if (sleep_for > 0) hidpp_plat_sleep_ms((uint32_t)sleep_for);
        uint8_t noise[20] = { 0x11, 0x01, 0x41, 0x00 };
        size_t cpy = cap < 20 ? cap : 20;
        memcpy(buf, noise, cpy);
        return (int)cpy;
    }
    if (dev->rx_count == 0) {
        mock_unlock();
        if (timeout_ms > 0) hidpp_plat_sleep_ms((uint32_t)(timeout_ms > 10 ? 10 : timeout_ms));
        return 0;
    }
    mock_frame_t f = dev->rx[dev->rx_head];
    dev->rx_head = (dev->rx_head + 1) % 32;
    dev->rx_count--;
    mock_unlock();

    int cpy = f.len > (int)cap ? (int)cap : f.len;
    memcpy(buf, f.data, (size_t)cpy);
    return cpy;
}

/* ---- Tests ---- */

static void test_pid_classification(void) {
    assert(hidpp_classify_pid(0xC54D) == HIDPP_KIND_LIGHTSPEED_RECEIVER);
    assert(hidpp_classify_pid(0xC547) == HIDPP_KIND_LIGHTSPEED_RECEIVER);
    assert(hidpp_classify_pid(0xC539) == HIDPP_KIND_LIGHTSPEED_RECEIVER);
    assert(hidpp_classify_pid(0xC545) == HIDPP_KIND_LIGHTSPEED_RECEIVER);
    assert(hidpp_classify_pid(0xC53F) == HIDPP_KIND_LIGHTSPEED_RECEIVER);
    assert(hidpp_classify_pid(0xC541) == HIDPP_KIND_LIGHTSPEED_RECEIVER);

    assert(hidpp_classify_pid(0xC09B) == HIDPP_KIND_DIRECT_WIRED);
    assert(hidpp_classify_pid(0xC094) == HIDPP_KIND_DIRECT_WIRED);
    assert(hidpp_classify_pid(0xC08B) == HIDPP_KIND_DIRECT_WIRED);

    assert(hidpp_classify_pid(0xC548) == HIDPP_KIND_MULTI_RECEIVER);
    assert(hidpp_classify_pid(0xC52B) == HIDPP_KIND_MULTI_RECEIVER);
    assert(hidpp_classify_pid(0xC532) == HIDPP_KIND_MULTI_RECEIVER);
    assert(hidpp_classify_pid(0xC534) == HIDPP_KIND_MULTI_RECEIVER);
    printf("[PASS] test_pid_classification\n");
}

static void test_strict_frame_matching_and_drain(void) {
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock0");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 3;
    m->battery = 87;
    m->reg00_readable = 1;
    snprintf(m->dev_name, sizeof(m->dev_name), "G PRO X SUPERLIGHT 2 DEX");

    /* Pre-queue a stale bogus frame that MUST be drained before sending */
    uint8_t stale[20] = { 0x11, 0x01, 0x05, 0x5A, 0, 0x99, 0x99 };
    memcpy(m->stale[0].data, stale, 20);
    m->stale[0].len = 20;
    m->stale_count = 1;

    /* Inject unrelated prefix frames (wrong swid, 0x41 notification) before real reply */
    uint8_t wrong_swid[20] = { 0x11, 0x01, 0x05, 0x51, 0, 0x12, 0x34 };
    uint8_t notif_41[7]    = { 0x10, 0x01, 0x41, 0x04, 0x00, 0x00, 0x00 };
    memcpy(m->prefix[0].data, wrong_swid, 20);
    m->prefix[0].len = 20;
    memcpy(m->prefix[1].data, notif_41, 7);
    m->prefix[1].len = 7;
    m->prefix_count = 2;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    assert(m->drain_calls > 0);
    assert(m->notif_reg_writes == 1);

    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 1);
    assert(mousedpi_snap_perm_denied() == 0);
    assert(mousedpi_snap_dpi_x() == 1600);
    assert(mousedpi_snap_lod() == 3);
    assert(mousedpi_snap_battery() == 87);
    char *name = mousedpi_snap_device_name_dup();
    assert(name != NULL);
    assert(strcmp(name, "G PRO X SUPERLIGHT 2 DEX") == 0);
    free(name);

    mousedpi_stop_waits();
    printf("[PASS] test_strict_frame_matching_and_drain\n");
}

static void test_fast_fail_10_8f_and_ff_and_single_deadline(void) {
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_err");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->offline_10_8f = 1;

    uint64_t t0 = hidpp_plat_now_ms();
    int32_t ok = mousedpi_init_and_restore();
    uint64_t dt = hidpp_plat_now_ms() - t0;
    assert(ok == 0);
    assert(dt < 150); /* Instant fail on 0x10 0x8F, zero 350ms timeout wait */
    mousedpi_stop_waits();

    /* HID++ 2.0 0xFF error: also fail fast */
    mock_reset();
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_ff");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->hidpp2_ff_err = 1;

    t0 = hidpp_plat_now_ms();
    ok = mousedpi_init_and_restore();
    dt = hidpp_plat_now_ms() - t0;
    assert(ok == 0);
    assert(dt < 150);
    mousedpi_stop_waits();

    /* Single monotonic deadline ~350ms under continuous unrelated noise (NOT 8 * 350ms) */
    mock_reset();
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_noise");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->noise_forever = 1;

    t0 = hidpp_plat_now_ms();
    ok = mousedpi_init_and_restore();
    dt = hidpp_plat_now_ms() - t0;
    assert(ok == 0);
    assert(dt >= 300 && dt < 900);
    mousedpi_stop_waits();
    printf("[PASS] test_fast_fail_10_8f_and_ff_and_single_deadline\n");
}

static void test_truncated_dpi_resp_rejected(void) {
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_trunc");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 3200;
    m->dpi_y = 3200;
    m->lod = 1;
    m->truncate_dpi_resp = 1; /* 10 bytes < 14 bytes required */

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 0);
    assert(mousedpi_get_connected() == 0);
    mousedpi_stop_waits();
    printf("[PASS] test_truncated_dpi_resp_rejected\n");
}

static void test_multi_candidate_probe_and_perm_denied(void) {
    mock_reset();
    /* Candidate 0: LIGHTSPEED keyboard (no 0x2202)
     * Candidate 1: Permission denied wired device
     * Candidate 2: Multi-receiver Bolt with mouse on slot 3 */
    g_mock_dev_count = 3;

    snprintf(g_mock_devs[0].id, sizeof(g_mock_devs[0].id), "/dev/hidraw0");
    g_mock_devs[0].vid = 0x046D;
    g_mock_devs[0].pid = 0xC547;
    g_mock_devs[0].kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    g_mock_devs[0].has_dpi_feature = 0;

    snprintf(g_mock_devs[1].id, sizeof(g_mock_devs[1].id), "/dev/hidraw1");
    g_mock_devs[1].vid = 0x046D;
    g_mock_devs[1].pid = 0xC09B;
    g_mock_devs[1].kind = HIDPP_KIND_DIRECT_WIRED;
    g_mock_devs[1].perm_denied = 1;

    snprintf(g_mock_devs[2].id, sizeof(g_mock_devs[2].id), "/dev/hidraw2");
    g_mock_devs[2].vid = 0x046D;
    g_mock_devs[2].pid = 0xC548;
    g_mock_devs[2].kind = HIDPP_KIND_MULTI_RECEIVER;
    g_mock_devs[2].has_dpi_feature = 1;
    g_mock_devs[2].active_slot = 3;
    g_mock_devs[2].dpi_x = 2400;
    g_mock_devs[2].dpi_y = 2400;
    g_mock_devs[2].lod = 2;
    g_mock_devs[2].battery = 55;
    g_mock_devs[2].name_fail = 1; /* Keep default name */

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 1);
    assert(mousedpi_snap_perm_denied() == 0);
    assert(mousedpi_snap_dpi_x() == 2400);
    assert(mousedpi_snap_battery() == 55);
    char *name = mousedpi_snap_device_name_dup();
    assert(name != NULL && strcmp(name, HIDPP_DEFAULT_DEVICE_NAME) == 0);
    free(name);
    mousedpi_stop_waits();

    /* Now test all-permission-denied case */
    mock_reset();
    g_mock_dev_count = 1;
    snprintf(g_mock_devs[0].id, sizeof(g_mock_devs[0].id), "/dev/hidraw9");
    g_mock_devs[0].vid = 0x046D;
    g_mock_devs[0].pid = 0xC09B;
    g_mock_devs[0].kind = HIDPP_KIND_DIRECT_WIRED;
    g_mock_devs[0].perm_denied = 1;

    ok = mousedpi_init_and_restore();
    assert(ok == 0);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 0);
    assert(mousedpi_snap_perm_denied() == 1);
    mousedpi_stop_waits();
    printf("[PASS] test_multi_candidate_probe_and_perm_denied\n");
}

static void test_direct_wired_no_receiver_notif_and_wake_restore(void) {
    mock_reset();
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = 1200;
    g_mock_cfg_lod = 1;

    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/wired0");
    m->vid = 0x046D;
    m->pid = 0xC09B;
    m->kind = HIDPP_KIND_DIRECT_WIRED;
    m->has_dpi_feature = 1;
    m->active_slot = 0xFF;
    m->dpi_x = 800; /* Drifted from saved 1200 */
    m->dpi_y = 800;
    m->lod = 2;
    m->battery = 100;
    snprintf(m->dev_name, sizeof(m->dev_name), "PRO X2 WIRED");

    /* init_and_restore should restore 1200 / lod 1 */
    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    assert(m->notif_reg_writes == 0); /* Direct wired: do not send receiver 0x10 0xFF 0x80 */
    assert(mousedpi_get_dpi_x() == 1200);
    assert(mousedpi_get_lod() == 1);

    /* Simulate mouse sleep-resetting its DPI back to 800, then trigger wake restore */
    int32_t rev_before = mousedpi_state_rev();
    mock_lock();
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 2;
    mock_unlock();

    mousedpi_request_wake_restore();
    int32_t rev_after = mousedpi_wait_rev(rev_before);
    assert(rev_after > rev_before);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_dpi_x() == 1200);
    assert(mousedpi_snap_lod() == 1);

    /* Also test request_set updates config & bumps rev */
    int32_t rev2 = mousedpi_state_rev();
    mousedpi_request_set(3200, 3);
    int32_t rev3 = mousedpi_wait_rev(rev2);
    assert(rev3 > rev2);
    assert(mousedpi_get_dpi_x() == 3200);
    assert(mousedpi_get_lod() == 3);
    assert(g_mock_cfg_dpi == 3200 && g_mock_cfg_lod == 3);

    mousedpi_stop_waits();
    /* wait_rev after stop_waits must return -1 immediately */
    assert(mousedpi_wait_rev(rev3) == -1);
    printf("[PASS] test_direct_wired_no_receiver_notif_and_wake_restore\n");
}

static void test_report_descriptor_parser(void) {
    /* 1. Logitech HID++ descriptor with Usage Page 0xFF00, Report ID 0x10 (7B) & 0x11 (20B),
     *    plus Push/Pop around a nested Generic Desktop section. */
    static const uint8_t logi_desc[] = {
        0x06, 0x00, 0xFF,       /* Usage Page (Vendor 0xFF00) */
        0x09, 0x01,             /* Usage (0x01) */
        0xA1, 0x01,             /* Collection (Application) */
        0xA4,                   /*   Push (saves Usage Page 0xFF00) */
        0x05, 0x01,             /*   Usage Page (Generic Desktop) */
        0x75, 0x08,             /*   Report Size (8) */
        0x95, 0x03,             /*   Report Count (3) */
        0xB4,                   /*   Pop (restores Usage Page 0xFF00) */
        0x85, 0x10,             /*   Report ID (0x10) */
        0x75, 0x08,             /*   Report Size (8) */
        0x95, 0x06,             /*   Report Count (6) -> 1 + 6 = 7B */
        0x81, 0x00,             /*   Input */
        0x91, 0x00,             /*   Output */
        0x85, 0x11,             /*   Report ID (0x11) */
        0x75, 0x08,             /*   Report Size (8) */
        0x95, 0x13,             /*   Report Count (19) -> 1 + 19 = 20B */
        0x81, 0x00,             /*   Input */
        0x91, 0x00,             /*   Output */
        0xC0                    /* End Collection */
    };
    hidpp_rdesc_caps_t caps;
    hidpp_parse_report_descriptor(logi_desc, sizeof(logi_desc), &caps);
    assert(caps.has_ff00 == 1);
    assert(caps.in_bytes_10 == 7);
    assert(caps.out_bytes_10 == 7);
    assert(caps.in_bytes_11 == 20);
    assert(caps.out_bytes_11 == 20);

    /* 2. Standard boot mouse (no 0xFF00) */
    static const uint8_t mouse_desc[] = {
        0x05, 0x01, /* Usage Page (Generic Desktop) */
        0x09, 0x02, /* Usage (Mouse) */
        0xA1, 0x01, /* Collection (Application) */
        0x75, 0x08, /* Report Size (8) */
        0x95, 0x04, /* Report Count (4) */
        0x81, 0x02, /* Input */
        0xC0
    };
    hidpp_parse_report_descriptor(mouse_desc, sizeof(mouse_desc), &caps);
    assert(caps.has_ff00 == 0);
    assert(caps.out_bytes_11 == 0);

    /* 3. Truncated / long-item malformed descriptor (must not crash or trigger UBSan) */
    static const uint8_t bad_desc[] = { 0xFE, 0x02, 0x01, 0xAA, 0xBB, 0x06, 0x00 };
    hidpp_parse_report_descriptor(bad_desc, sizeof(bad_desc), &caps);
    assert(caps.has_ff00 == 0);

    printf("[PASS] test_report_descriptor_parser\n");
}

static void test_posix_config_atomic(void) {
#if defined(_WIN32)
    char tmp_dir[MAX_PATH];
    DWORD tn = GetTempPathA(MAX_PATH, tmp_dir);
    assert(tn > 0 && tn < MAX_PATH);
    char cfg[MAX_PATH + 64];
    snprintf(cfg, sizeof(cfg), "%smousedpi_test_%lu.ini", tmp_dir, (unsigned long)GetCurrentProcessId());
    (void)DeleteFileA(cfg);

    int ok_w = hidpp_posix_config_write_file(cfg, 1600, 3);
    assert(ok_w == 1);
    int32_t dpi = 0, lod = 0;
    int ok_r = hidpp_posix_config_read_file(cfg, &dpi, &lod);
    assert(ok_r == 1);
    assert(dpi == 1600 && lod == 3);

    /* Parse hand-edited INI with comments & mixed case */
    FILE *f = fopen(cfg, "wb");
    assert(f != NULL);
    fprintf(f, "# comment\n[Mouse]\n  DPI = 4000 \n  LOD = 99\n");
    fclose(f);
    ok_r = hidpp_posix_config_read_file(cfg, &dpi, &lod);
    assert(ok_r == 1);
    assert(dpi == 4000 && lod == HIDPP_DEFAULT_LOD);
    (void)DeleteFileA(cfg);
#else
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/mousedpi_test_%ld", (long)getpid());
    char cfg[300];
    snprintf(cfg, sizeof(cfg), "%s/nested/config.ini", dir);

    int ok_w = hidpp_posix_config_write_file(cfg, 1600, 3);
    assert(ok_w == 1);

    int32_t dpi = 0, lod = 0;
    int ok_r = hidpp_posix_config_read_file(cfg, &dpi, &lod);
    assert(ok_r == 1);
    assert(dpi == 1600 && lod == 3);

    /* Overwrite atomically */
    ok_w = hidpp_posix_config_write_file(cfg, 800, 1);
    assert(ok_w == 1);
    ok_r = hidpp_posix_config_read_file(cfg, &dpi, &lod);
    assert(ok_r == 1);
    assert(dpi == 800 && lod == 1);

    /* Parse hand-edited INI with comments & mixed case */
    FILE *f = fopen(cfg, "wb");
    assert(f != NULL);
    fprintf(f, "# comment\n[Mouse]\n  DPI = 4000 \n  LOD = 99\n");
    fclose(f);
    ok_r = hidpp_posix_config_read_file(cfg, &dpi, &lod);
    assert(ok_r == 1);
    assert(dpi == 4000 && lod == HIDPP_DEFAULT_LOD);

    /* Cleanup */
    unlink(cfg);
    char nested[280];
    snprintf(nested, sizeof(nested), "%s/nested", dir);
    rmdir(nested);
    rmdir(dir);
#endif
    printf("[PASS] test_posix_config_atomic\n");
}

/* ---- Concurrent snapshot consistency stress test ---- */

static hidpp_mutex_t g_stress_lock;
static int           g_stress_lock_inited = 0;
static int           g_stress_done = 0;

static void stress_set_done(int v) {
    if (!g_stress_lock_inited) {
        hidpp_mutex_init(&g_stress_lock);
        g_stress_lock_inited = 1;
    }
    hidpp_mutex_lock(&g_stress_lock);
    g_stress_done = v;
    hidpp_mutex_unlock(&g_stress_lock);
}

static int stress_is_done(void) {
    hidpp_mutex_lock(&g_stress_lock);
    int v = g_stress_done;
    hidpp_mutex_unlock(&g_stress_lock);
    return v;
}

static void snapshot_writer_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 2000; i++) {
        hidpp_state_t st;
        init_default_state(&st);
        if (i & 1) {
            st.connected = 1;
            st.perm_denied = 0;
            st.dpi_x = 1600;
            st.dpi_y = 1600;
            st.lod = 3;
            st.battery = 88;
            snprintf(st.device_name, sizeof(st.device_name), "STATE_A");
        } else {
            st.connected = 0;
            st.perm_denied = 1;
            st.dpi_x = 800;
            st.dpi_y = 800;
            st.lod = 1;
            st.battery = 15;
            snprintf(st.device_name, sizeof(st.device_name), "STATE_B");
        }
        commit_state(&st, 1);
    }
    stress_set_done(1);
}

static void test_concurrent_snapshots(void) {
    mock_reset();
    ensure_core_init();
    stress_set_done(0);

    hidpp_thread_t th;
    int started = hidpp_thread_start(&th, snapshot_writer_thread, NULL);
    assert(started == 1);
    while (!stress_is_done()) {
        mousedpi_snapshot_capture();
        int32_t rev  = mousedpi_snap_rev();
        int32_t conn = mousedpi_snap_connected();
        int32_t perm = mousedpi_snap_perm_denied();
        int32_t dpi  = mousedpi_snap_dpi_x();
        int32_t lod  = mousedpi_snap_lod();
        int32_t bat  = mousedpi_snap_battery();
        char *name   = mousedpi_snap_device_name_dup();
        assert(name != NULL);
        assert(rev >= 0);

        if (strcmp(name, "STATE_A") == 0) {
            assert(conn == 1 && perm == 0 && dpi == 1600 && lod == 3 && bat == 88);
        } else if (strcmp(name, "STATE_B") == 0) {
            assert(conn == 0 && perm == 1 && dpi == 800 && lod == 1 && bat == 15);
        }
        free(name);
    }
    hidpp_thread_join(th);
    printf("[PASS] test_concurrent_snapshots\n");
}

/* ---- Round 1 regression tests ---- */

static void test_perm_denied_cleared_when_candidate_opened_offline(void) {
    mock_reset();
    g_mock_dev_count = 2;

    /* Candidate 0 opens fine, mouse is offline (0x10 0x8F) */
    snprintf(g_mock_devs[0].id, sizeof(g_mock_devs[0].id), "/dev/hidraw0");
    g_mock_devs[0].vid = 0x046D;
    g_mock_devs[0].pid = 0xC54D;
    g_mock_devs[0].kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    g_mock_devs[0].offline_10_8f = 1;

    /* Candidate 1 has perm_denied */
    snprintf(g_mock_devs[1].id, sizeof(g_mock_devs[1].id), "/dev/hidraw1");
    g_mock_devs[1].vid = 0x046D;
    g_mock_devs[1].pid = 0xC09B;
    g_mock_devs[1].kind = HIDPP_KIND_DIRECT_WIRED;
    g_mock_devs[1].perm_denied = 1;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 0);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 0);
    assert(mousedpi_snap_perm_denied() == 0);
    mousedpi_stop_waits();
    printf("[PASS] test_perm_denied_cleared_when_candidate_opened_offline\n");
}

static void test_fn6_zero_ack_readback_and_no_zero_config(void) {
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_zero_ack");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 2;
    m->battery = 90;
    m->fn6_zero_ack = 1;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);

    int32_t rev0 = mousedpi_state_rev();
    mousedpi_request_set_seq(7, 2400, 3);
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);

    mousedpi_snapshot_capture();
    assert(mousedpi_snap_rev() == rev1);
    assert(mousedpi_snap_applied_seq() == 7);
    assert(mousedpi_snap_dpi_x() == 2400);
    assert(mousedpi_snap_lod() == 3);
    assert(g_mock_cfg_dpi == 2400 && g_mock_cfg_lod == 3);
    mousedpi_stop_waits();
    printf("[PASS] test_fn6_zero_ack_readback_and_no_zero_config\n");
}

static void test_fn6_rejected_advances_applied_seq_without_config_write(void) {
    mock_reset();
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = 1600;
    g_mock_cfg_lod = 2;

    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_reject");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->battery = 75;
    m->fn6_reject_ff = 1;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    int cfg_writes_before = g_mock_cfg_writes;

    int32_t rev0 = mousedpi_state_rev();
    mousedpi_request_set_seq(42, 3200, 3);
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);

    mousedpi_snapshot_capture();
    assert(mousedpi_snap_applied_seq() == 42);
    assert(mousedpi_snap_dpi_x() == 1600);
    assert(g_mock_cfg_writes == cfg_writes_before);
    assert(g_mock_cfg_dpi == 1600);
    mousedpi_stop_waits();
    printf("[PASS] test_fn6_rejected_advances_applied_seq_without_config_write\n");
}

static void test_fn1_caps_no_indep_y_no_lod_no_rewrite_loop(void) {
    mock_reset();
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = 2000;
    g_mock_cfg_lod = 3;

    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/mock_caps");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 2;
    m->battery = 80;
    m->caps_present = 1;
    m->caps_num_levels = 3; /* Distractor in r[5]: 0b11 != r[6]=0x00 */
    m->caps_flags = 0x00;   /* r[6]: No independent Y, no LOD */
    m->fn5_lod_zero = 1;
    m->reject_nonzero_y_lod = 1;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    assert(mousedpi_get_dpi_x() == 2000);
    assert(m->fn6_writes == 1);
    assert(m->last_fn6_y == 0 && m->last_fn6_lod == 0);

    /* Subsequent fast ensure must NOT rewrite fn=6 */
    hidpp_mutex_lock(&g_hid_lock);
    int changed = 0;
    int fast_ok = ensure_dpi_fast_locked(0, &changed);
    hidpp_mutex_unlock(&g_hid_lock);
    assert(fast_ok == 1);
    assert(m->fn6_writes == 1);
    mousedpi_stop_waits();
    printf("[PASS] test_fn1_caps_no_indep_y_no_lod_no_rewrite_loop\n");
}

static void test_wake_rev_bump_does_not_clobber_concurrent_state(void) {
    mock_reset();
    ensure_core_init();

    hidpp_state_t st;
    init_default_state(&st);
    st.connected = 1;
    st.dpi_x = 3200;
    st.dpi_y = 3200;
    st.lod = 3;
    st.battery = 99;
    snprintf(st.device_name, sizeof(st.device_name), "FRESH_STATE");
    commit_state_seq(&st, 1, 11);

    int32_t rev_before = mousedpi_state_rev();
    bump_state_rev_only();

    mousedpi_snapshot_capture();
    assert(mousedpi_snap_rev() == rev_before + 1);
    assert(mousedpi_snap_applied_seq() == 11);
    assert(mousedpi_snap_dpi_x() == 3200);
    assert(mousedpi_snap_battery() == 99);
    char *name = mousedpi_snap_device_name_dup();
    assert(name != NULL && strcmp(name, "FRESH_STATE") == 0);
    free(name);
    mousedpi_stop_waits();
    printf("[PASS] test_wake_rev_bump_does_not_clobber_concurrent_state\n");
}

static void test_topology_change_hotplug_no_config_and_notify_reconnect(void) {
    /* Part 1: Hotplug when no config exists (g_has_saved == 0) */
    mock_reset();
    g_mock_dev_count = 0;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 0);
    int32_t rev0 = mousedpi_state_rev();

    mock_lock();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/hotplug0");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1800;
    m->dpi_y = 1800;
    m->lod = 2;
    m->battery = 66;
    snprintf(m->dev_name, sizeof(m->dev_name), "HOTPLUG MOUSE");
    mock_unlock();

    hidpp_core_signal_topology_change();
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 1);
    assert(mousedpi_snap_dpi_x() == 1800);
    mousedpi_stop_waits();

    /* Part 2: Notify thread 0x41 wake + disconnect reconnect */
    mock_reset();
    g_mock_needs_notify = 1;
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = 2200;
    g_mock_cfg_lod = 1;

    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/notify0");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 2200;
    m->dpi_y = 2200;
    m->lod = 1;
    m->battery = 80;

    ok = mousedpi_init_and_restore();
    assert(ok == 1);

    /* Simulate mouse sleep drift + 0x41 wake notification */
    int32_t r_before = mousedpi_state_rev();
    mock_lock();
    m->dpi_x = 800;
    m->dpi_y = 800;
    mock_unlock();
    uint8_t wake41[7] = { 0x10, 0x01, 0x41, 0x04, 0x00, 0x00, 0x00 };
    mock_push_notify(wake41, 7);

    int32_t r_after = mousedpi_wait_rev(r_before);
    assert(r_after > r_before);
    assert(mousedpi_get_dpi_x() == 2200);

    /* Simulate notify read error (disconnect) -> topology change signal */
    r_before = mousedpi_state_rev();
    mock_lock();
    g_mock_notify_err = 1;
    mock_unlock();
    r_after = mousedpi_wait_rev(r_before);
    assert(r_after > r_before);

    mousedpi_stop_waits();
    printf("[PASS] test_topology_change_hotplug_no_config_and_notify_reconnect\n");
}

static void test_rapid_start_stop_lifecycle_stress(void) {
    for (int iter = 0; iter < 15; iter++) {
        mock_reset();
        g_mock_dev_count = 1;
        mock_device_t *m = &g_mock_devs[0];
        snprintf(m->id, sizeof(m->id), "/dev/life0");
        m->vid = 0x046D;
        m->pid = 0xC09B;
        m->kind = HIDPP_KIND_DIRECT_WIRED;
        m->has_dpi_feature = 1;
        m->active_slot = 0xFF;
        m->dpi_x = 1600;
        m->dpi_y = 1600;
        m->lod = 2;
        m->battery = 100;

        int32_t ok = mousedpi_init_and_restore();
        assert(ok == 1);
        mousedpi_request_refresh();
        mousedpi_request_wake_restore();
        mousedpi_stop_waits();

        /* After stop_waits, calling state_rev / wait_rev must not call hidpp_plat_init again */
        mock_lock();
        int inits_at_stop = g_mock_plat_inits;
        mock_unlock();

        int32_t rv = mousedpi_state_rev();
        assert(rv >= 0);
        int32_t wv = mousedpi_wait_rev(rv);
        assert(wv == -1);
        mock_lock();
        assert(g_mock_plat_inits == inits_at_stop);
        mock_unlock();
    }
    printf("[PASS] test_rapid_start_stop_lifecycle_stress\n");
}

static void test_pid_c5xx_wildcard_ble_slot0_and_reg00_rmw(void) {
    /* Unlisted 0xC5xx PID must classify as MULTI_RECEIVER */
    assert(hidpp_classify_pid(0xC599) == HIDPP_KIND_MULTI_RECEIVER);
    assert(hidpp_classify_pid(0xC501) == HIDPP_KIND_MULTI_RECEIVER);

    /* Register 0x00 read-modify-write must preserve existing bits */
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/rmw0");
    m->vid = 0x046D;
    m->pid = 0xC599;
    m->kind = hidpp_classify_pid(0xC599);
    m->has_dpi_feature = 1;
    m->active_slot = 2;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->battery = 70;
    m->reg00_readable = 1;
    m->reg00_init[0] = 0xAA;
    m->reg00_init[1] = 0x20;
    m->reg00_init[2] = 0x55;

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    assert(m->notif_reg_writes == 1);
    assert(m->reg00_written[0] == 0xAA);
    assert(m->reg00_written[1] == 0x21); /* 0x20 | 0x01 */
    assert(m->reg00_written[2] == 0x55);
    mousedpi_stop_waits();

    /* BLE direct device on slot 0x00 */
    mock_reset();
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/ble0");
    m->vid = 0x046D;
    m->pid = 0xB034;
    m->kind = hidpp_classify_pid(0xB034);
    assert(m->kind == HIDPP_KIND_DIRECT_WIRED);
    m->has_dpi_feature = 1;
    m->active_slot = 0x00;
    m->dpi_x = 1400;
    m->dpi_y = 1400;
    m->lod = 2;
    m->battery = 60;

    ok = mousedpi_init_and_restore();
    assert(ok == 1);
    assert(mousedpi_get_dpi_x() == 1400);
    mousedpi_stop_waits();
    printf("[PASS] test_pid_c5xx_wildcard_ble_slot0_and_reg00_rmw\n");
}

static void test_utf8_device_name_and_device_switch_reset(void) {
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/utf8_dev1");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->battery = 92;
    snprintf(m->dev_name, sizeof(m->dev_name), "\xE7\xBD\x97\xE6\x8A\x80 PRO X2");

    int32_t ok = mousedpi_init_and_restore();
    assert(ok == 1);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_battery() == 92);
    char *name1 = mousedpi_snap_device_name_dup();
    assert(name1 != NULL && strcmp(name1, "\xE7\xBD\x97\xE6\x8A\x80 PRO X2") == 0);
    free(name1);

    /* Switch to device 2 with no battery feature and failing device name */
    mock_lock();
    snprintf(m->id, sizeof(m->id), "/dev/dev2_minimal");
    m->no_battery = 1;
    m->no_onboard = 1;
    m->name_fail = 1;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    mock_unlock();

    int32_t rev0 = mousedpi_state_rev();
    hidpp_core_signal_topology_change();
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);

    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 1);
    assert(mousedpi_snap_battery() == -1);
    char *name2 = mousedpi_snap_device_name_dup();
    assert(name2 != NULL && strcmp(name2, HIDPP_DEFAULT_DEVICE_NAME) == 0);
    free(name2);

    assert(mousedpi_wait_tray_ready(50) == 1);
    mousedpi_stop_waits();
    printf("[PASS] test_utf8_device_name_and_device_switch_reset\n");
}

/* ---- Round 2 regression tests ---- */

static void test_fn1_caps_r6_offset_and_retry_on_failure(void) {
    /* Case A: (r[5]=3, r[6]=0x00) -> fn=6 must send y=0, lod=0 */
    mock_reset();
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = 2400;
    g_mock_cfg_lod = 3;
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/caps_a");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 2;
    m->caps_present = 1;
    m->caps_num_levels = 3; /* 0b11 distractor in r[5] */
    m->caps_flags = 0x00;   /* r[6] = no indep Y, no LOD */

    assert(mousedpi_init_and_restore() == 1);
    assert(m->fn6_writes == 1);
    assert(m->last_fn6_x == 2400);
    assert(m->last_fn6_y == 0);
    assert(m->last_fn6_lod == 0);
    mousedpi_stop_waits();

    /* Case B: (r[5]=5, r[6]=0x03) -> fn=6 must send y=dpi, lod!=0 */
    mock_reset();
    g_mock_cfg_has = 1;
    g_mock_cfg_dpi = 3200;
    g_mock_cfg_lod = 3;
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/caps_b");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 1;
    m->caps_present = 1;
    m->caps_num_levels = 5; /* 0b101 distractor in r[5] (bit1 == 0) */
    m->caps_flags = 0x03;   /* r[6] = indep Y + LOD */

    assert(mousedpi_init_and_restore() == 1);
    assert(m->fn6_writes == 1);
    assert(m->last_fn6_x == 3200);
    assert(m->last_fn6_y == 3200);
    assert(m->last_fn6_lod == 3);
    mousedpi_stop_waits();

    /* Case C: fn=1 failure must NOT permanently set g_cached_caps_read = 1 */
    mock_reset();
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/caps_retry");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 2;
    m->caps_present = 0; /* First call fails with HID++ 2.0 0xFF error */

    assert(mousedpi_init_and_restore() == 1);
    hidpp_mutex_lock(&g_hid_lock);
    assert(g_cached_caps_read == 0);
    hidpp_mutex_unlock(&g_hid_lock);

    /* Now enable fn=1 with (r[5]=3, r[6]=0x00) and trigger a set: must retry fn=1 and honor r[6]=0x00 */
    mock_lock();
    m->caps_present = 1;
    m->caps_num_levels = 3;
    m->caps_flags = 0x00;
    mock_unlock();

    int32_t rev0 = mousedpi_state_rev();
    mousedpi_request_set_seq(1, 1600, 3);
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);
    hidpp_mutex_lock(&g_hid_lock);
    assert(g_cached_caps_read == 1);
    assert(g_cached_has_indep_y == 0);
    assert(g_cached_has_lod == 0);
    hidpp_mutex_unlock(&g_hid_lock);
    assert(m->last_fn6_y == 0 && m->last_fn6_lod == 0);
    mousedpi_stop_waits();
    printf("[PASS] test_fn1_caps_r6_offset_and_retry_on_failure\n");
}

static void test_reg00_read_fail_zero_write_and_write_8f_retry(void) {
    /* (a1) 0x81 returns 0x8F error -> zero 0x80 writes, g_notif_enabled_gen stays 0 */
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/reg00_err");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->reg00_readable = 0;

    assert(mousedpi_init_and_restore() == 1);
    assert(m->notif_reg_writes == 0);
    hidpp_mutex_lock(&g_hid_lock);
    assert(g_notif_enabled_gen == 0);
    hidpp_mutex_unlock(&g_hid_lock);
    mousedpi_stop_waits();

    /* (a2) 0x81 times out -> zero 0x80 writes, g_notif_enabled_gen stays 0 */
    mock_reset();
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/reg00_tmo");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->reg00_read_timeout = 1;

    assert(mousedpi_init_and_restore() == 1);
    assert(m->notif_reg_writes == 0);
    hidpp_mutex_lock(&g_hid_lock);
    assert(g_notif_enabled_gen == 0);
    hidpp_mutex_unlock(&g_hid_lock);
    mousedpi_stop_waits();

    /* (b) 0x81 succeeds, 0x80 returns 0x8F -> g_notif_enabled_gen stays 0, retries on reconnect */
    mock_reset();
    g_mock_dev_count = 1;
    m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/reg00_werr");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->reg00_readable = 1;
    m->reg00_init[0] = 0x10;
    m->reg00_init[1] = 0x04;
    m->reg00_init[2] = 0x20;
    m->reg00_write_err = 1;

    assert(mousedpi_init_and_restore() == 1);
    assert(m->notif_reg_writes == 1);
    hidpp_mutex_lock(&g_hid_lock);
    assert(g_notif_enabled_gen == 0);
    hidpp_mutex_unlock(&g_hid_lock);

    /* Next reconnect (topology change) with write now succeeding -> must retry and mark enabled */
    mock_lock();
    m->reg00_write_err = 0;
    mock_unlock();
    int32_t rev0 = mousedpi_state_rev();
    hidpp_core_signal_topology_change();
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);
    hidpp_mutex_lock(&g_hid_lock);
    assert(g_notif_enabled_gen == g_endpoint_gen && g_notif_enabled_gen != 0);
    hidpp_mutex_unlock(&g_hid_lock);
    assert(m->notif_reg_writes == 2);
    assert(m->reg00_written[0] == 0x10 && m->reg00_written[1] == 0x05 && m->reg00_written[2] == 0x20);
    mousedpi_stop_waits();
    printf("[PASS] test_reg00_read_fail_zero_write_and_write_8f_retry\n");
}

static void test_fn6_always_fn5_readback_ignores_fake_echo(void) {
    mock_reset();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/fn6_readback");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->lod = 2;
    m->battery = 85;
    m->fn6_fake_echo = 1;
    m->fn6_fake_x = 2400;
    m->fn6_fake_y = 2400;
    m->fn6_fake_lod = 3;
    m->hw_clamp_x = 2350; /* Sensor actually clamped to 2350 / lod 2 */
    m->hw_clamp_y = 2350;
    m->hw_clamp_lod = 2;

    assert(mousedpi_init_and_restore() == 1);

    /* 1. set_dpi_locked must ignore non-zero fake echo and store fn=5 hardware value */
    int32_t rev0 = mousedpi_state_rev();
    mousedpi_request_set_seq(5, 2400, 3);
    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);

    mousedpi_snapshot_capture();
    assert(mousedpi_snap_applied_seq() == 5);
    assert(mousedpi_snap_dpi_x() == 2350);
    assert(mousedpi_snap_lod() == 2);
    assert(g_mock_cfg_dpi == 2350 && g_mock_cfg_lod == 2);

    /* 2. ensure_dpi_fast_locked must also ignore fake echo and use fn=5 readback */
    mock_lock();
    m->dpi_x = 800;
    m->dpi_y = 800;
    m->hw_clamp_x = 2300;
    m->hw_clamp_y = 2300;
    m->hw_clamp_lod = 1;
    m->fn6_fake_x = 2350;
    m->fn6_fake_y = 2350;
    m->fn6_fake_lod = 2;
    mock_unlock();

    hidpp_mutex_lock(&g_hid_lock);
    int changed = 0;
    assert(ensure_dpi_fast_locked(0, &changed) == 1);
    assert(changed == 1);
    assert(g_saved_dpi == 2300 && g_saved_lod == 1);
    hidpp_mutex_unlock(&g_hid_lock);
    assert(mousedpi_get_dpi_x() == 2300);
    assert(mousedpi_get_lod() == 1);

    mousedpi_stop_waits();
    printf("[PASS] test_fn6_always_fn5_readback_ignores_fake_echo\n");
}

static void stop_waits_worker(void *unused) {
    (void)unused;
    mousedpi_stop_waits();
}

static void test_concurrent_stop_waits_single_ownership(void) {
    for (int iter = 0; iter < 10; iter++) {
        mock_reset();
        g_mock_needs_notify = 1;
        g_mock_dev_count = 1;
        mock_device_t *m = &g_mock_devs[0];
        snprintf(m->id, sizeof(m->id), "/dev/cstop0");
        m->vid = 0x046D;
        m->pid = 0xC54D;
        m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
        m->has_dpi_feature = 1;
        m->active_slot = 0x01;
        m->dpi_x = 1600;
        m->dpi_y = 1600;
        m->lod = 2;

        assert(mousedpi_init_and_restore() == 1);
        mousedpi_request_refresh();
        mousedpi_request_wake_restore();

        hidpp_thread_t t1, t2;
        assert(hidpp_thread_start(&t1, stop_waits_worker, NULL) == 1);
        assert(hidpp_thread_start(&t2, stop_waits_worker, NULL) == 1);
        mousedpi_stop_waits();
        hidpp_thread_join(t1);
        hidpp_thread_join(t2);
    }
    printf("[PASS] test_concurrent_stop_waits_single_ownership\n");
}

static void test_notify_first_open_from_empty_and_no_unrelated_flicker(void) {
    /* Part 1: Notify thread starts with 0 devices (saw_no_dev=1), then device appears -> triggers topology refresh */
    mock_reset();
    g_mock_needs_notify = 1;
    g_mock_dev_count = 0;

    assert(mousedpi_init_and_restore() == 0);
    int32_t rev0 = mousedpi_state_rev();
    /* Let hid_notify_main observe empty device list once */
    hidpp_plat_sleep_ms(60u);

    mock_lock();
    g_mock_dev_count = 1;
    mock_device_t *m = &g_mock_devs[0];
    snprintf(m->id, sizeof(m->id), "/dev/first_open0");
    m->vid = 0x046D;
    m->pid = 0xC54D;
    m->kind = HIDPP_KIND_LIGHTSPEED_RECEIVER;
    m->has_dpi_feature = 1;
    m->active_slot = 0x01;
    m->dpi_x = 1600;
    m->dpi_y = 1600;
    m->lod = 2;
    m->battery = 77;
    snprintf(m->dev_name, sizeof(m->dev_name), "REAL MOUSE NAME");
    mock_unlock();

    int32_t rev1 = mousedpi_wait_rev(rev0);
    assert(rev1 > rev0);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 1);
    assert(mousedpi_snap_dpi_x() == 1600);
    assert(mousedpi_snap_battery() == 77);
    char *n1 = mousedpi_snap_device_name_dup();
    assert(n1 != NULL && strcmp(n1, "REAL MOUSE NAME") == 0);
    free(n1);

    /* Part 2: Unrelated topology change with same (endpoint_id, dev_idx) must NOT flicker battery/name */
    mock_lock();
    m->no_battery = 1;
    m->name_fail = 1;
    mock_unlock();
    int32_t rev2 = mousedpi_state_rev();
    hidpp_core_signal_topology_change();
    int32_t rev3 = mousedpi_wait_rev(rev2);
    assert(rev3 > rev2);
    mousedpi_snapshot_capture();
    assert(mousedpi_snap_connected() == 1);
    assert(mousedpi_snap_battery() == 77);
    char *n2 = mousedpi_snap_device_name_dup();
    assert(n2 != NULL && strcmp(n2, "REAL MOUSE NAME") == 0);
    free(n2);

    mousedpi_stop_waits();
    printf("[PASS] test_notify_first_open_from_empty_and_no_unrelated_flicker\n");
}

int main(void) {
    test_pid_classification();
    test_strict_frame_matching_and_drain();
    test_fast_fail_10_8f_and_ff_and_single_deadline();
    test_truncated_dpi_resp_rejected();
    test_multi_candidate_probe_and_perm_denied();
    test_direct_wired_no_receiver_notif_and_wake_restore();
    test_report_descriptor_parser();
    test_posix_config_atomic();
    test_concurrent_snapshots();
    test_perm_denied_cleared_when_candidate_opened_offline();
    test_fn6_zero_ack_readback_and_no_zero_config();
    test_fn6_rejected_advances_applied_seq_without_config_write();
    test_fn1_caps_no_indep_y_no_lod_no_rewrite_loop();
    test_wake_rev_bump_does_not_clobber_concurrent_state();
    test_topology_change_hotplug_no_config_and_notify_reconnect();
    test_rapid_start_stop_lifecycle_stress();
    test_pid_c5xx_wildcard_ble_slot0_and_reg00_rmw();
    test_utf8_device_name_and_device_switch_reset();
    test_fn1_caps_r6_offset_and_retry_on_failure();
    test_reg00_read_fail_zero_write_and_write_8f_retry();
    test_fn6_always_fn5_readback_ignores_fake_echo();
    test_concurrent_stop_waits_single_ownership();
    test_notify_first_open_from_empty_and_no_unrelated_flicker();
    printf("ALL HID++ CORE TESTS PASSED!\n");
    return 0;
}
