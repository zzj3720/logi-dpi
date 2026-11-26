#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
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
    int                   truncate_dpi_resp;
    int                   offline_10_8f;
    int                   hidpp2_ff_err;
    int                   noise_forever;
    int                   notif_reg_writes;
    int                   drain_calls;

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

void hidpp_plat_init(void) {}
void hidpp_plat_start_watchers(void) {}
void hidpp_plat_stop_watchers(void) {}
int  hidpp_plat_needs_notify_thread(void) { return 0; }

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

    /* HID++ 1.0 wireless notification register 0x00 enable */
    if (len == HIDPP_SHORT_LEN && buf[0] == 0x10 && buf[1] == 0xFF && buf[2] == 0x80 && buf[3] == 0x00) {
        m->notif_reg_writes++;
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
        else if (fid == 0x1004) r[4] = 0x06;
        else if (fid == 0x8100) r[4] = 0x07;
        else if (fid == 0x0005) r[4] = 0x08;
        else r[4] = 0x00;
        dev_push_rx(dev, r, 20);
    } else if (feat_idx == 0x05 && fn == 5) {
        /* 0x2202 getSensorDpi */
        r[5]  = (uint8_t)((m->dpi_x >> 8) & 0xFF);
        r[6]  = (uint8_t)(m->dpi_x & 0xFF);
        r[9]  = (uint8_t)((m->dpi_y >> 8) & 0xFF);
        r[10] = (uint8_t)(m->dpi_y & 0xFF);
        r[13] = (uint8_t)m->lod;
        dev_push_rx(dev, r, m->truncate_dpi_resp ? 10 : 20);
    } else if (feat_idx == 0x05 && fn == 6) {
        /* 0x2202 setSensorDpi */
        m->dpi_x = ((int32_t)buf[5] << 8) | buf[6];
        m->dpi_y = ((int32_t)buf[7] << 8) | buf[8];
        m->lod   = buf[9];
        r[5] = buf[5];
        r[6] = buf[6];
        r[7] = buf[7];
        r[8] = buf[8];
        r[9] = buf[9];
        dev_push_rx(dev, r, 20);
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

    assert(hidpp_posix_config_write_file(cfg, 1600, 3) == 1);
    int32_t dpi = 0, lod = 0;
    assert(hidpp_posix_config_read_file(cfg, &dpi, &lod) == 1);
    assert(dpi == 1600 && lod == 3);

    /* Parse hand-edited INI with comments & mixed case */
    FILE *f = fopen(cfg, "wb");
    assert(f != NULL);
    fprintf(f, "# comment\n[Mouse]\n  DPI = 4000 \n  LOD = 99\n");
    fclose(f);
    assert(hidpp_posix_config_read_file(cfg, &dpi, &lod) == 1);
    assert(dpi == 4000 && lod == HIDPP_DEFAULT_LOD);
    (void)DeleteFileA(cfg);
#else
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/mousedpi_test_%ld", (long)getpid());
    char cfg[300];
    snprintf(cfg, sizeof(cfg), "%s/nested/config.ini", dir);

    assert(hidpp_posix_config_write_file(cfg, 1600, 3) == 1);

    int32_t dpi = 0, lod = 0;
    assert(hidpp_posix_config_read_file(cfg, &dpi, &lod) == 1);
    assert(dpi == 1600 && lod == 3);

    /* Overwrite atomically */
    assert(hidpp_posix_config_write_file(cfg, 800, 1) == 1);
    assert(hidpp_posix_config_read_file(cfg, &dpi, &lod) == 1);
    assert(dpi == 800 && lod == 1);

    /* Parse hand-edited INI with comments & mixed case */
    FILE *f = fopen(cfg, "wb");
    assert(f != NULL);
    fprintf(f, "# comment\n[Mouse]\n  DPI = 4000 \n  LOD = 99\n");
    fclose(f);
    assert(hidpp_posix_config_read_file(cfg, &dpi, &lod) == 1);
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

static volatile int g_stress_done = 0;

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
    g_stress_done = 1;
}

static void test_concurrent_snapshots(void) {
    mock_reset();
    ensure_core_init();
    g_stress_done = 0;

    hidpp_thread_t th;
    assert(hidpp_thread_start(&th, snapshot_writer_thread, NULL) == 1);
    while (!g_stress_done) {
        mousedpi_snapshot_capture();
        int32_t conn = mousedpi_snap_connected();
        int32_t perm = mousedpi_snap_perm_denied();
        int32_t dpi  = mousedpi_snap_dpi_x();
        int32_t lod  = mousedpi_snap_lod();
        int32_t bat  = mousedpi_snap_battery();
        char *name   = mousedpi_snap_device_name_dup();
        assert(name != NULL);

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
    printf("ALL HID++ CORE TESTS PASSED!\n");
    return 0;
}
