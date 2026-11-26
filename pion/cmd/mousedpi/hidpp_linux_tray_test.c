#define _GNU_SOURCE
#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in real hidpp_linux.c with ONLY the two D-Bus SNI probes mocked so we exercise
 * the real mousedpi_wait_tray_ready / mousedpi_notify_tray_show / linux_tray_show_idle_cb
 * and real GLib g_idle_add -> g_main_context_default() cross-thread dispatch. */
#define HIDPP_LINUX_TRAY_TEST 1
#include "hidpp_linux.c"

static pthread_mutex_t g_test_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_mock_host_rc = 1;
static int             g_mock_item_rc = 0;
static int             g_item_polls = 0;
static int             g_host_polls = 0;

static pthread_t       g_main_tid;
static int             g_show_count = 0;
static int             g_show_on_main_thread = 0;
static int             g_queued_ordinary_msgs = 0;

/* Strong symbol overriding hidpp_linux.c's weak declaration of Invar's mb_host_window_show. */
void mb_host_window_show(void) {
    pthread_mutex_lock(&g_test_lock);
    g_show_count++;
    if (pthread_equal(pthread_self(), g_main_tid)) {
        g_show_on_main_thread++;
    }
    pthread_mutex_unlock(&g_test_lock);
}

int hidpp_linux_test_sni_host(void) {
    pthread_mutex_lock(&g_test_lock);
    g_host_polls++;
    int rc = g_mock_host_rc;
    pthread_mutex_unlock(&g_test_lock);
    return rc;
}

int hidpp_linux_test_sni_item(const char *needle) {
    pthread_mutex_lock(&g_test_lock);
    g_item_polls++;
    int rc = (needle && strstr(needle, "org.kde.StatusNotifierItem-") != NULL) ? g_mock_item_rc : 0;
    pthread_mutex_unlock(&g_test_lock);
    return rc;
}

static void reset_test_state(int host_rc, int item_rc) {
    pthread_mutex_lock(&g_test_lock);
    g_mock_host_rc = host_rc;
    g_mock_item_rc = item_rc;
    g_host_polls = 0;
    g_item_polls = 0;
    g_show_count = 0;
    g_show_on_main_thread = 0;
    g_queued_ordinary_msgs = 0;
    pthread_mutex_unlock(&g_test_lock);
}

static int get_show_count(void) {
    pthread_mutex_lock(&g_test_lock);
    int v = g_show_count;
    pthread_mutex_unlock(&g_test_lock);
    return v;
}

static int get_show_on_main(void) {
    pthread_mutex_lock(&g_test_lock);
    int v = g_show_on_main_thread;
    pthread_mutex_unlock(&g_test_lock);
    return v;
}

/* Pump GLib's default GMainContext on the main thread (identical to Invar linux_app.c run_loop)
 * WITHOUT ever drawing a GTK frame or draining queued ordinary Invar messages. */
static int pump_glib_until_shown(int expected_count, uint64_t timeout_ms) {
    GMainContext *ctx = g_main_context_default();
    uint64_t deadline = hidpp_plat_now_ms() + timeout_ms;
    while (hidpp_plat_now_ms() < deadline) {
        while (g_main_context_iteration(ctx, FALSE)) {
        }
        if (get_show_count() >= expected_count) {
            return 1;
        }
        hidpp_plat_sleep_ms(5u);
    }
    while (g_main_context_iteration(ctx, FALSE)) {
    }
    return get_show_count() >= expected_count;
}

static void *worker_startup_timeout(void *arg) {
    int *out_ready = (int *)arg;
    int ready = mousedpi_wait_tray_ready(80);
    *out_ready = ready;
    if (!ready) {
        mousedpi_notify_tray_show();
    }
    return NULL;
}

static void *worker_post_startup_disappear(void *arg) {
    int *out_watch_rc = (int *)arg;
    int ready = mousedpi_wait_tray_ready(200);
    if (!ready) {
        *out_watch_rc = -99;
        return NULL;
    }
    int watch_rc = mousedpi_wait_tray_ready(-1);
    *out_watch_rc = watch_rc;
    if (watch_rc == 0) {
        mousedpi_notify_tray_show();
    }
    return NULL;
}

static void *worker_notify_with_ordinary_msgs(void *arg) {
    (void)arg;
    hidpp_plat_sleep_ms(10u);
    mousedpi_notify_tray_show();
    return NULL;
}

/* Case 1: Startup SNI registration timeout -> hidden window reveals directly on GLib main thread with 0 frames. */
static void test_startup_sni_timeout(void) {
    reset_test_state(/*host_rc=*/1, /*item_rc=*/0);
    int ready = -1;
    pthread_t th;
    if (pthread_create(&th, NULL, worker_startup_timeout, &ready) != 0) abort();

    int shown = pump_glib_until_shown(1, 1500u);
    pthread_join(th, NULL);

    if (ready != 0 || !shown || get_show_count() != 1 || get_show_on_main() != 1) {
        fprintf(stderr, "FAIL test_startup_sni_timeout: ready=%d shown=%d count=%d main=%d\n",
                ready, shown, get_show_count(), get_show_on_main());
        exit(1);
    }
    printf("PASS test_startup_sni_timeout\n");
}

/* Case 2: Post-startup SNI disappearance -> ignores transient D-Bus error (-1), reveals on definite absence (0). */
static void test_post_startup_sni_disappear(void) {
    reset_test_state(/*host_rc=*/1, /*item_rc=*/1);
    int watch_rc = -1;
    pthread_t th;
    if (pthread_create(&th, NULL, worker_post_startup_disappear, &watch_rc) != 0) abort();

    /* Wait until initial registration succeeds and watcher enters -1 poll loop. */
    uint64_t wait_dl = hidpp_plat_now_ms() + 1000u;
    while (hidpp_plat_now_ms() < wait_dl) {
        pthread_mutex_lock(&g_test_lock);
        int polls = g_item_polls;
        pthread_mutex_unlock(&g_test_lock);
        if (polls >= 2) break;
        hidpp_plat_sleep_ms(5u);
    }

    /* Inject transient D-Bus timeout (-1): must NOT trigger window show. */
    pthread_mutex_lock(&g_test_lock);
    g_mock_host_rc = -1;
    pthread_mutex_unlock(&g_test_lock);

    if (pump_glib_until_shown(1, 80u)) {
        fprintf(stderr, "FAIL test_post_startup_sni_disappear: transient D-Bus error must not pop window\n");
        exit(1);
    }

    /* Now inject definite SNI item disappearance (host=1, item=0): must reveal on main thread. */
    pthread_mutex_lock(&g_test_lock);
    g_mock_host_rc = 1;
    g_mock_item_rc = 0;
    pthread_mutex_unlock(&g_test_lock);

    int shown = pump_glib_until_shown(1, 1500u);
    pthread_join(th, NULL);

    if (watch_rc != 0 || !shown || get_show_count() != 1 || get_show_on_main() != 1) {
        fprintf(stderr, "FAIL test_post_startup_sni_disappear: watch_rc=%d shown=%d count=%d main=%d\n",
                watch_rc, shown, get_show_count(), get_show_on_main());
        exit(1);
    }
    printf("PASS test_post_startup_sni_disappear\n");
}

/* Case 3: Ordinary host messages already queued ahead (never drained while hidden with 0 frames)
 * must NOT block tray show recovery on the GLib main loop. */
static void test_recovery_with_pending_ordinary_messages(void) {
    reset_test_state(/*host_rc=*/1, /*item_rc=*/0);
    pthread_mutex_lock(&g_test_lock);
    g_queued_ordinary_msgs = 3; /* simulated non-empty messages_in with no GTK frames to drain them */
    pthread_mutex_unlock(&g_test_lock);

    pthread_t th;
    if (pthread_create(&th, NULL, worker_notify_with_ordinary_msgs, NULL) != 0) abort();

    int shown = pump_glib_until_shown(1, 1000u);
    pthread_join(th, NULL);

    pthread_mutex_lock(&g_test_lock);
    int still_queued = g_queued_ordinary_msgs;
    pthread_mutex_unlock(&g_test_lock);

    if (!shown || get_show_count() != 1 || get_show_on_main() != 1 || still_queued != 3) {
        fprintf(stderr, "FAIL test_recovery_with_pending_ordinary_messages: shown=%d count=%d main=%d queued=%d\n",
                shown, get_show_count(), get_show_on_main(), still_queued);
        exit(1);
    }
    printf("PASS test_recovery_with_pending_ordinary_messages\n");
}

/* Case 4: Shutdown guard — once mousedpi_stop_waits() has run, tray recovery must not show window. */
static void test_shutdown_blocks_tray_show(void) {
    reset_test_state(/*host_rc=*/1, /*item_rc=*/0);
    mousedpi_stop_waits();
    mousedpi_notify_tray_show();

    if (pump_glib_until_shown(1, 60u) || get_show_count() != 0) {
        fprintf(stderr, "FAIL test_shutdown_blocks_tray_show: count=%d\n", get_show_count());
        exit(1);
    }
    printf("PASS test_shutdown_blocks_tray_show\n");
}

int main(void) {
    g_main_tid = pthread_self();
    test_startup_sni_timeout();
    test_post_startup_sni_disappear();
    test_recovery_with_pending_ordinary_messages();
    test_shutdown_blocks_tray_show();
    printf("All 4 Linux tray GLib idle recovery tests passed.\n");
    return 0;
}
