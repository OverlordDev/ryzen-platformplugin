/*
 * ryzend - userspace brain for RyzenPlatform.kext.
 *
 *   ryzend --status                 print kext topology/status and exit
 *   ryzend --reset                  hand every CPU back to the firmware and exit
 *   ryzend --poke CPU low|nom|high [SECONDS]
 *                                   THE first experiment: drive ONE logical CPU to a level
 *                                   for SECONDS (default 5), heartbeating, then restore.
 *                                   Watch `ryzenstat -w 8` in another terminal: effMHz of
 *                                   that CPU must move, the others must not.
 *   ryzend --shadow [--csv f]       DEFAULT. compute decisions, log them, write nothing
 *   ryzend --apply  [--csv f]       send decisions to the kext (root, kext booted with
 *                                   ryzenplatform_write=1)
 *
 * Options: --hot-cpu N   logical CPU to treat as the "hot" one (default: kext's
 *                        preferred core)
 *          --smt-stride K  sibling of cpu c is c^K when K=1, i.e. adjacent pairs
 *                        (default 1; 0 = no SMT siblings)
 *          --no-hid      do not install the CGEventTap (no predictor)
 *
 * Threads:
 *   main    CFRunLoop: CGEventTap (HID), sleep/wake notifications, signals.
 *   policy  waits on a condition variable; wakes on HID events or its own
 *           timeout (50 ms when idle, 4 ms when active). Idle must stay idle:
 *           this process is supposed to make the machine cooler, not hotter.
 *
 * Safety (mirrors docs/SAFETY.md):
 *   - shadow is the default; --apply must be asked for;
 *   - heartbeat every 50 ms while applying; the kext reverts on 150 ms of silence;
 *   - reset before sleep, re-arm only after wake;
 *   - SIGINT/SIGTERM/exit => resetAuto; if this process is killed -9 the kext
 *     reverts on its own (connection close + watchdog);
 *   - the kext owns the thermal guard; the daemon only follows it.
 *
 * Written from memory without a compiler on macOS. See docs/HANDOFF.md for the
 * list of calls that are most likely to need a fix on the first build.
 */
#include <stdatomic.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <dispatch/dispatch.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <sys/resource.h>
#include <libproc.h>

#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include "../shared/RyzenPlatformAPI.h"

#define MAX_CPUS RYZEN_PLATFORM_MAX_CORES

/* ------------------------------------------------------------------ time */

static mach_timebase_info_data_t g_tb;

static uint64_t
now_ns(void)
{
    return mach_absolute_time() * g_tb.numer / g_tb.denom;
}

static uint64_t
abs_to_ns(uint64_t t)
{
    return t * g_tb.numer / g_tb.denom;
}

static void
sleep_ms_timed(pthread_cond_t *cv, pthread_mutex_t *mu, uint32_t ms)
{
    /* Caller holds mu. Returns on signal or timeout. */
    struct timespec rel = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    pthread_cond_timedwait_relative_np(cv, mu, &rel);
}

/* ---------------------------------------------------------------- config */

typedef struct {
    bool     apply;
    bool     hid;
    int      hot_cpu;          /* -1 = take the kext's preferred core */
    int      smt_stride;       /* 1 = siblings are adjacent (c ^ 1), 0 = none */
    uint32_t burst_ms;
    uint32_t ready_ms;
    uint32_t deep_idle_ms;
    uint32_t interactive_ms;
    uint32_t down_dwell_ms;    /* minimum time at a level before stepping down */
    const char *csv_path;
} config_t;

static config_t g_cfg = {
    .apply = false, .hid = true, .hot_cpu = -1, .smt_stride = 1,
    .burst_ms = 3, .ready_ms = 500, .deep_idle_ms = 500, .interactive_ms = 1500,
    .down_dwell_ms = 50, .csv_path = NULL,
};

/* ----------------------------------------------------------------- kext */

static io_connect_t g_conn = IO_OBJECT_NULL;
static ryzen_topology_t g_topo;
static bool g_have_kext = false;

static bool
kext_open(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("com_illia_RyzenPlatform"));
    if (svc == IO_OBJECT_NULL) {
        fprintf(stderr, "ryzend: RyzenPlatform.kext is not loaded\n");
        return false;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "ryzend: IOServiceOpen failed: 0x%x (%s)\n", kr,
                geteuid() == 0 ? "is another client attached?" : "run as root");
        return false;
    }
    return true;
}

static void
kext_close(void)
{
    if (g_conn != IO_OBJECT_NULL) {
        IOServiceClose(g_conn);
        g_conn = IO_OBJECT_NULL;
    }
}

/* Getter with the client's ABI version as the single scalar input. */
static kern_return_t
kext_get(uint32_t selector, void *out, size_t outsz)
{
    uint64_t in[1] = { RYZEN_PLATFORM_ABI_VERSION };
    size_t sz = outsz;
    return IOConnectCallMethod(g_conn, selector, in, 1, NULL, 0, NULL, NULL, out, &sz);
}

static kern_return_t
kext_reset(void)
{
    uint64_t in[1] = { RYZEN_PLATFORM_ABI_VERSION };
    return IOConnectCallMethod(g_conn, kRyzenSelectorResetAuto, in, 1, NULL, 0, NULL, NULL, NULL, NULL);
}

static kern_return_t
kext_heartbeat(uint32_t seq)
{
    ryzen_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    hb.abi_version = RYZEN_PLATFORM_ABI_VERSION;
    hb.struct_size = sizeof hb;
    hb.sequence    = seq;
    hb.daemon_pid  = (uint32_t)getpid();
    return IOConnectCallStructMethod(g_conn, kRyzenSelectorHeartbeat, &hb, sizeof hb, NULL, NULL);
}

static kern_return_t
kext_apply(const ryzen_apply_t *a)
{
    return IOConnectCallStructMethod(g_conn, kRyzenSelectorApplyPerf, a, sizeof *a, NULL, NULL);
}

static void
print_last_error(void)
{
    ryzen_error_t e;
    if (kext_get(kRyzenSelectorGetLastError, &e, sizeof e) == KERN_SUCCESS && e.code != 0) {
        fprintf(stderr, "ryzend: kext says: code %u cpu %u: %s\n", e.code, e.core, e.message);
    }
}

static bool
topology_refresh(void)
{
    if (!g_have_kext) {
        return false;
    }
    if (kext_get(kRyzenSelectorGetTopology, &g_topo, sizeof g_topo) != KERN_SUCCESS) {
        fprintf(stderr, "ryzend: getTopology failed (ABI mismatch?)\n");
        return false;
    }
    return g_topo.core_count > 0 && g_topo.core_count <= MAX_CPUS;
}

/* --------------------------------------------------------------- signals */

static _Atomic uint64_t g_last_input_ns;     /* HID tap writes, policy reads */
static bool g_apply_mode;                    /* --apply was requested: only then do we ever reset the kext */
static _Atomic int      g_sleeping;
static _Atomic int      g_stop;
static _Atomic int      g_wake_epoch;        /* bumps on wake: re-read topology */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static void
poke_policy(void)
{
    pthread_mutex_lock(&g_mu);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* --- HID: CGEventTap, listen-only. Needs Accessibility / Input Monitoring. */

static CGEventRef
hid_cb(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ref)
{
    (void)proxy; (void)ref;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        return ev;       /* TODO(handoff): re-enable the tap with CGEventTapEnable */
    }
    /* A mouse moves hundreds of times a second; wake the policy thread at most every 2 ms. */
    uint64_t now = now_ns();
    uint64_t prev = atomic_exchange(&g_last_input_ns, now);
    if (now - prev > 2000000ull) {
        poke_policy();
    }
    return ev;
}

static bool
hid_install(void)
{
    CGEventMask mask = CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventRightMouseDown) |
                       CGEventMaskBit(kCGEventScrollWheel)   | CGEventMaskBit(kCGEventKeyDown) |
                       CGEventMaskBit(kCGEventMouseMoved)    | CGEventMaskBit(kCGEventLeftMouseDragged);
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                         kCGEventTapOptionListenOnly, mask, hid_cb, NULL);
    if (!tap) {
        fprintf(stderr, "ryzend: no event tap (grant Accessibility/Input Monitoring); "
                        "running without the HID predictor\n");
        return false;
    }
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    CFRelease(src);
    return true;
}

/* --- WindowServer CPU time: "is a frame being drawn right now?" */

static pid_t g_ws_pid;
static uint64_t g_ws_prev_ns;
static bool g_ws_have_prev;

static pid_t
find_pid_by_name(const char *name)
{
    int bytes = proc_listallpids(NULL, 0);
    if (bytes <= 0) return 0;
    pid_t *pids = malloc((size_t)bytes + 64 * sizeof(pid_t));
    if (!pids) return 0;
    int got = proc_listallpids(pids, bytes + 64 * (int)sizeof(pid_t));
    pid_t found = 0;
    for (int i = 0; i < got / (int)sizeof(pid_t) && !found; i++) {
        char nm[64];
        if (pids[i] > 0 && proc_name(pids[i], nm, sizeof nm) > 0 && strcmp(nm, name) == 0) {
            found = pids[i];
        }
    }
    free(pids);
    return found;
}

/* True if WindowServer used measurable CPU since the last call. */
static bool
frame_pending_poll(void)
{
    if (g_ws_pid == 0) {
        g_ws_pid = find_pid_by_name("WindowServer");
        if (g_ws_pid == 0) return false;
    }
    struct rusage_info_v2 ri;
    if (proc_pid_rusage(g_ws_pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0) {
        g_ws_pid = 0;                     /* restarted or no permission: look again next time */
        g_ws_have_prev = false;
        return false;
    }
    uint64_t cur = abs_to_ns(ri.ri_user_time + ri.ri_system_time);
    bool active = false;
    if (g_ws_have_prev) {
        active = (cur - g_ws_prev_ns) > 150000ull;      /* >150 us of CPU since last poll */
    }
    g_ws_prev_ns = cur;
    g_ws_have_prev = true;
    return active;
}

/* --- CPU load */

static uint32_t *g_prev_busy, *g_prev_total;
static uint32_t g_prev_n;

/* Mean load over all logical CPUs since the previous call, 0.0 .. 1.0. */
static double
cpu_load_poll(void)
{
    natural_t n = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t cnt = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS) {
        return 0.0;
    }
    if (!g_prev_busy || g_prev_n != n) {
        free(g_prev_busy); free(g_prev_total);
        g_prev_busy  = calloc(n, sizeof(uint32_t));
        g_prev_total = calloc(n, sizeof(uint32_t));
        g_prev_n = n;
    }
    double busy_sum = 0, total_sum = 0;
    processor_cpu_load_info_t li = (processor_cpu_load_info_t)info;
    for (natural_t i = 0; i < n; i++) {
        uint32_t busy  = li[i].cpu_ticks[CPU_STATE_USER] + li[i].cpu_ticks[CPU_STATE_SYSTEM] +
                         li[i].cpu_ticks[CPU_STATE_NICE];
        uint32_t total = busy + li[i].cpu_ticks[CPU_STATE_IDLE];
        busy_sum  += (uint32_t)(busy  - g_prev_busy[i]);       /* unsigned wrap is fine */
        total_sum += (uint32_t)(total - g_prev_total[i]);
        g_prev_busy[i] = busy;
        g_prev_total[i] = total;
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
    return total_sum > 0 ? busy_sum / total_sum : 0.0;
}

/* ---------------------------------------------------------------- policy */

typedef enum { ST_DEEP_IDLE, ST_IDLE_READY, ST_BURST, ST_INTERACTIVE, ST_SUSTAINED, ST_THERMAL } state_t;
typedef enum { LV_LOW = 0, LV_NOM = 1, LV_HIGH = 2 } level_t;

static const char *
state_name(state_t s)
{
    static const char *n[] = { "DEEP_IDLE", "IDLE_READY", "BURST", "INTERACTIVE", "SUSTAINED", "THERMAL_GUARD" };
    return n[s];
}

typedef struct {
    state_t state;
    level_t level[MAX_CPUS];
    uint8_t epp;
} decision_t;

typedef struct {
    uint64_t burst_until, ui_until, last_input, sustained_since;
    bool     was_sustained;
} policy_t;

static int
sibling_of(int cpu)
{
    return g_cfg.smt_stride ? (cpu ^ 1) : -1;
}

/* Mirror of Policy._decide() in sim/policy_sim.py. Keep the two in step. */
static void
policy_decide(policy_t *p, uint64_t now, bool frame_pending, double load, uint32_t guard,
              uint64_t input_ns, int hot, decision_t *d)
{
    uint32_t n = g_topo.core_count;
    int sib = sibling_of(hot);

    memset(d, 0, sizeof *d);
    for (uint32_t i = 0; i < n; i++) d->level[i] = LV_LOW;

    if (input_ns && input_ns > p->last_input) {
        p->last_input  = input_ns;
        p->burst_until = input_ns + (uint64_t)g_cfg.burst_ms * 1000000ull;
        p->ui_until    = input_ns + (uint64_t)g_cfg.interactive_ms * 1000000ull;
    }
    if (frame_pending && p->ui_until < now + (uint64_t)g_cfg.interactive_ms * 1000000ull) {
        p->last_input = now;
        p->ui_until   = now + (uint64_t)g_cfg.interactive_ms * 1000000ull;
    }

    /* Heavy sustained load for >1 s => stop being clever, let PPT be the ceiling. */
    if (load > 0.60) {
        if (!p->was_sustained) { p->was_sustained = true; p->sustained_since = now; }
    } else {
        p->was_sustained = false;
    }
    bool sustained = p->was_sustained && (now - p->sustained_since) > 1000000000ull;

    if (sustained) {
        d->state = ST_SUSTAINED; d->epp = 128;
        for (uint32_t i = 0; i < n; i++) d->level[i] = LV_HIGH;
    } else if (now < p->burst_until) {
        d->state = ST_BURST; d->epp = 0;
        d->level[hot] = LV_HIGH;
        if (sib >= 0 && (uint32_t)sib < n) d->level[sib] = LV_HIGH;
    } else if (frame_pending || now < p->ui_until) {
        d->state = ST_INTERACTIVE; d->epp = 32;
        d->level[hot] = LV_HIGH;
        if (sib >= 0 && (uint32_t)sib < n) d->level[sib] = LV_NOM;
    } else if (now - p->last_input < (uint64_t)g_cfg.ready_ms * 1000000ull && p->last_input) {
        d->state = ST_IDLE_READY; d->epp = 200;
        d->level[hot] = LV_NOM;
    } else {
        d->state = ST_DEEP_IDLE; d->epp = 255;
    }

    /* The kext enforces the guard itself; following it here only keeps our log honest
     * and avoids asking for something the kext will clamp anyway. */
    if (guard >= 1) {
        d->state = ST_THERMAL; d->epp = 255;
        for (uint32_t i = 0; i < n; i++) {
            if (guard >= 2 || d->level[i] > LV_NOM) d->level[i] = guard >= 2 ? LV_LOW : LV_NOM;
        }
    }
}

/* Per-CPU level => CPPC request. HIGH asks for highest and lets the SMU/PPT decide. */
static ryzen_perf_request_t
level_to_request(uint32_t cpu, level_t lv, uint8_t epp)
{
    ryzen_perf_request_t r;
    memset(&r, 0, sizeof r);
    r.core = cpu;
    r.epp  = epp;
    switch (lv) {
    case LV_LOW:  r.min_perf = g_topo.lowest_perf;  r.max_perf = g_topo.lowest_perf;  r.desired_perf = g_topo.lowest_perf;  break;
    case LV_NOM:  r.min_perf = g_topo.lowest_perf;  r.max_perf = g_topo.nominal_perf; r.desired_perf = g_topo.nominal_perf; break;
    case LV_HIGH: r.min_perf = g_topo.nominal_perf; r.max_perf = g_topo.highest_perf; r.desired_perf = g_topo.highest_perf; break;
    }
    r.ttl_ms = 0;
    return r;
}

/* ------------------------------------------------------------------- log */

static FILE *g_csv;

static void
csv_open(void)
{
    if (!g_cfg.csv_path) return;
    g_csv = fopen(g_cfg.csv_path, "w");
    if (!g_csv) { perror("ryzend: csv"); return; }
    fprintf(g_csv, "t_ms,state,input_age_ms,frame_pending,load,guard,tctl_c,hot_level,epp,applied\n");
}

/* ------------------------------------------------------------ policy loop */

static void *
policy_thread(void *arg)
{
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    policy_t pol;
    memset(&pol, 0, sizeof pol);

    uint64_t t0 = now_ns();
    uint64_t last_hb = 0, last_status = 0, last_load = 0, last_log = 0;
    uint32_t hb_seq = 0;
    double load = 0.0;
    uint32_t guard = 0;
    int32_t temp = RYZEN_TEMP_UNKNOWN;
    bool frame_pending = false;
    int epoch = atomic_load(&g_wake_epoch);

    level_t cur_level[MAX_CPUS];
    uint64_t level_since[MAX_CPUS];
    uint8_t  cur_epp = 255;
    memset(cur_level, 0, sizeof cur_level);
    memset(level_since, 0, sizeof level_since);

    ryzen_apply_t last_sent;
    memset(&last_sent, 0, sizeof last_sent);
    bool sent_valid = false;
    state_t last_state = ST_DEEP_IDLE;

    pthread_mutex_lock(&g_mu);
    while (!atomic_load(&g_stop)) {
        uint64_t now = now_ns();

        if (atomic_load(&g_sleeping)) {
            sent_valid = false;
            sleep_ms_timed(&g_cv, &g_mu, 200);
            continue;
        }
        if (epoch != atomic_load(&g_wake_epoch)) {
            epoch = atomic_load(&g_wake_epoch);
            sent_valid = false;
            if (g_have_kext) topology_refresh();
            memset(&pol, 0, sizeof pol);
        }

        /* --- gather (cheap, and only as often as it is useful) */
        if (now - last_load > 100000000ull) {
            load = cpu_load_poll();
            last_load = now;
        }
        frame_pending = frame_pending_poll();
        if (g_have_kext && now - last_status > 250000000ull) {
            ryzen_status_t st;
            if (kext_get(kRyzenSelectorGetStatus, &st, sizeof st) == KERN_SUCCESS) {
                guard = st.guard_level;
                temp  = st.tctl_millic;
                if (g_cfg.apply && (st.flags & RYZEN_FLAG_LOCKOUT)) {
                    /* The kext latched a safety stop. Do not fight it. */
                    fprintf(stderr, "ryzend: kext lockout latched; stopping applies (run --reset once safe)\n");
                    g_cfg.apply = false;
                }
            }
            last_status = now;
        }

        int hot = g_cfg.hot_cpu >= 0 ? g_cfg.hot_cpu : (int)g_topo.preferred_core;
        if (hot < 0 || (uint32_t)hot >= g_topo.core_count) hot = 0;

        /* --- decide */
        decision_t d;
        policy_decide(&pol, now, frame_pending, load, guard,
                      atomic_load(&g_last_input_ns), hot, &d);

        /* --- hysteresis: up immediately, down only after a dwell */
        for (uint32_t i = 0; i < g_topo.core_count; i++) {
            if (d.level[i] >= cur_level[i]) {
                if (d.level[i] != cur_level[i]) level_since[i] = now;
                cur_level[i] = d.level[i];
            } else if (now - level_since[i] >= (uint64_t)g_cfg.down_dwell_ms * 1000000ull) {
                cur_level[i] = d.level[i];
                level_since[i] = now;
            }
        }
        cur_epp = d.epp;

        /* --- act */
        bool applied = false;
        if (g_cfg.apply && g_have_kext) {
            ryzen_apply_t a;
            memset(&a, 0, sizeof a);
            a.abi_version = RYZEN_PLATFORM_ABI_VERSION;
            a.struct_size = sizeof a;
            a.count = g_topo.core_count;
            for (uint32_t i = 0; i < g_topo.core_count; i++) {
                a.req[i] = level_to_request(i, cur_level[i], cur_epp);
            }
            if (!sent_valid || memcmp(&a, &last_sent, sizeof a) != 0) {
                kern_return_t kr = kext_apply(&a);
                if (kr == KERN_SUCCESS) {
                    last_sent = a;
                    sent_valid = true;
                    applied = true;
                } else {
                    fprintf(stderr, "ryzend: applyPerf failed: 0x%x\n", kr);
                    print_last_error();
                    sent_valid = false;
                }
            }
            if (now - last_hb >= 50000000ull) {
                kext_heartbeat(hb_seq++);
                last_hb = now;
            }
        }

        /* --- log */
        bool changed = d.state != last_state;
        if (g_csv && (changed || applied || now - last_log > 1000000000ull)) {
            uint64_t in = atomic_load(&g_last_input_ns);
            fprintf(g_csv, "%llu,%s,%lld,%d,%.2f,%u,%.1f,%d,%u,%d\n",
                    (unsigned long long)((now - t0) / 1000000ull), state_name(d.state),
                    in ? (long long)((now - in) / 1000000ull) : -1LL, frame_pending ? 1 : 0, load,
                    guard, temp == RYZEN_TEMP_UNKNOWN ? -1.0 : temp / 1000.0,
                    (int)cur_level[hot], cur_epp, applied ? 1 : 0);
            fflush(g_csv);
            last_log = now;
        }
        if (changed && !g_csv) {
            fprintf(stderr, "ryzend: %s -> %s (load %.2f, guard %u)\n", state_name(last_state),
                    state_name(d.state), load, guard);
        }
        last_state = d.state;

        /* --- sleep until something happens. Idle must stay idle. */
        uint32_t wait_ms = (d.state == ST_DEEP_IDLE) ? 50 : 4;
        sleep_ms_timed(&g_cv, &g_mu, wait_ms);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* ------------------------------------------------------- sleep / wake */

static io_connect_t g_pm_root;

static void
power_cb(void *ref, io_service_t svc, natural_t msg, void *arg)
{
    (void)ref; (void)svc;
    switch (msg) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(g_pm_root, (intptr_t)arg);
        break;
    case kIOMessageSystemWillSleep:
        /* Stop touching the CPUs *before* the machine sleeps, then let it go. */
        atomic_store(&g_sleeping, 1);
        if (g_have_kext && g_apply_mode) {
            /* Taking g_mu waits out any iteration of the policy thread that is mid-apply,
             * so no apply can land after this reset. */
            pthread_mutex_lock(&g_mu);
            kext_reset();
            pthread_mutex_unlock(&g_mu);
        }
        IOAllowPowerChange(g_pm_root, (intptr_t)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        /* Re-arm only after wake: topology and CPPC state are re-read first. */
        atomic_fetch_add(&g_wake_epoch, 1);
        atomic_store(&g_sleeping, 0);
        poke_policy();
        break;
    default:
        break;
    }
}

static bool
power_install(void)
{
    IONotificationPortRef port = NULL;
    io_object_t notifier = IO_OBJECT_NULL;
    g_pm_root = IORegisterForSystemPower(NULL, &port, power_cb, &notifier);
    if (g_pm_root == MACH_PORT_NULL || !port) {
        fprintf(stderr, "ryzend: IORegisterForSystemPower failed\n");
        return false;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port),
                       kCFRunLoopCommonModes);
    return true;
}

/* ------------------------------------------------------------------ main */

static void
on_signal(void *ctx)
{
    (void)ctx;
    atomic_store(&g_stop, 1);
    poke_policy();
    CFRunLoopStop(CFRunLoopGetMain());
}

static void
install_signal(int sig)
{
    signal(sig, SIG_IGN);
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0,
                                                 dispatch_get_main_queue());
    dispatch_set_context(s, NULL);
    dispatch_source_set_event_handler_f(s, on_signal);
    dispatch_resume(s);
}

static int
cmd_status(void)
{
    if (!kext_open()) return 1;
    g_have_kext = true;
    if (!topology_refresh()) { kext_close(); return 1; }
    ryzen_status_t st;
    kext_get(kRyzenSelectorGetStatus, &st, sizeof st);

    printf("cpus %u   preferred core %u\n", g_topo.core_count, g_topo.preferred_core);
    printf("perf  lowest %u  nominal %u  highest %u   (est. %u / %u / %u MHz)\n",
           g_topo.lowest_perf, g_topo.nominal_perf, g_topo.highest_perf,
           g_topo.lowest_freq_mhz, g_topo.nominal_freq_mhz, g_topo.highest_freq_mhz);
    printf("cppc  supported %d  enabled on %u cpus\n", !!(g_topo.flags & RYZEN_FLAG_CPPC_SUPPORTED), g_topo.cppc_enabled);
    printf("flags 0x%x%s%s%s%s%s\n", st.flags,
           (st.flags & RYZEN_FLAG_WRITE_ENABLED) ? "  WRITE_ENABLED" : "  (read-only: boot with ryzenplatform_write=1)",
           (st.flags & RYZEN_FLAG_OVERRIDE_ACTIVE) ? "  OVERRIDE_ACTIVE" : "",
           (st.flags & RYZEN_FLAG_THERMAL_GUARD) ? "  THERMAL_GUARD" : "",
           (st.flags & RYZEN_FLAG_LOCKOUT) ? "  LOCKOUT" : "",
           (st.flags & RYZEN_FLAG_SLEEPING) ? "  SLEEPING" : "");
    if (st.tctl_millic == RYZEN_TEMP_UNKNOWN) printf("Tctl  unknown\n");
    else printf("Tctl  %.1f C   guard level %u\n", st.tctl_millic / 1000.0, st.guard_level);
    printf("watchdog resets %u   readback failures %u   smn retries %u\n",
           st.watchdog_resets, st.readback_failures, st.smn_retries);
    print_last_error();
    kext_close();
    return 0;
}

static int
cmd_reset(void)
{
    if (!kext_open()) return 1;
    g_have_kext = true;
    kern_return_t kr = kext_reset();
    printf("resetAuto: %s (0x%x)\n", kr == KERN_SUCCESS ? "ok" : "refused", kr);
    if (kr != KERN_SUCCESS) print_last_error();
    kext_close();
    return kr == KERN_SUCCESS ? 0 : 1;
}

/* One CPU, one level, a few seconds, then back to the firmware. Needs root + write boot-arg. */
static int
cmd_poke(int cpu, const char *lvname, int seconds)
{
    level_t lv;
    if (!strcmp(lvname, "low")) lv = LV_LOW;
    else if (!strcmp(lvname, "nom")) lv = LV_NOM;
    else if (!strcmp(lvname, "high")) lv = LV_HIGH;
    else { fprintf(stderr, "ryzend: level must be low|nom|high\n"); return 2; }
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;

    if (geteuid() != 0) { fprintf(stderr, "ryzend: --poke needs root\n"); return 1; }
    if (!kext_open()) return 1;
    g_have_kext = true;
    if (!topology_refresh()) { kext_close(); return 1; }
    if (cpu < 0 || (uint32_t)cpu >= g_topo.core_count) {
        fprintf(stderr, "ryzend: cpu %d out of range (0..%u)\n", cpu, g_topo.core_count - 1);
        kext_close();
        return 2;
    }
    if (!(g_topo.flags & RYZEN_FLAG_WRITE_ENABLED)) {
        fprintf(stderr, "ryzend: kext is read-only. Boot with ryzenplatform_write=1.\n");
        kext_close();
        return 1;
    }

    ryzen_apply_t a;
    memset(&a, 0, sizeof a);
    a.abi_version = RYZEN_PLATFORM_ABI_VERSION;
    a.struct_size = sizeof a;
    a.count = 1;
    a.req[0] = level_to_request((uint32_t)cpu, lv, lv == LV_LOW ? 255 : (lv == LV_HIGH ? 0 : 128));
    a.req[0].ttl_ms = (uint32_t)seconds * 1000 + 500;      /* the kext reverts this CPU by itself */

    printf("poke: cpu %d -> %s (perf min %u max %u des %u epp %u) for %d s\n", cpu, lvname,
           a.req[0].min_perf, a.req[0].max_perf, a.req[0].desired_perf, a.req[0].epp, seconds);

    kern_return_t kr = kext_apply(&a);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "ryzend: applyPerf failed: 0x%x\n", kr);
        print_last_error();
        kext_close();
        return 1;
    }
    for (int ms = 0, seq = 0; ms < seconds * 1000; ms += 50, seq++) {
        kext_heartbeat((uint32_t)seq);
        usleep(50 * 1000);
        if (ms % 1000 == 0) {
            ryzen_core_state_t cs;
            ryzen_status_t st;
            uint64_t in[2] = { RYZEN_PLATFORM_ABI_VERSION, (uint64_t)cpu };
            size_t sz = sizeof cs;
            if (IOConnectCallMethod(g_conn, kRyzenSelectorReadState, in, 2, NULL, 0, NULL, NULL, &cs, &sz) == KERN_SUCCESS &&
                kext_get(kRyzenSelectorGetStatus, &st, sizeof st) == KERN_SUCCESS) {
                printf("  t=%2ds  cpu%d REQ now: max %u min %u des %u epp %u   Tctl %.1f C   guard %u\n",
                       ms / 1000, cpu, cs.max_perf, cs.min_perf, cs.desired_perf, cs.epp,
                       st.tctl_millic == RYZEN_TEMP_UNKNOWN ? -1.0 : st.tctl_millic / 1000.0, st.guard_level);
                if (st.flags & RYZEN_FLAG_LOCKOUT) { fprintf(stderr, "ryzend: kext lockout, stopping\n"); break; }
            }
        }
    }
    kr = kext_reset();
    printf("poke: restored firmware settings (%s)\n", kr == KERN_SUCCESS ? "ok" : "check --status");
    kext_close();
    return kr == KERN_SUCCESS ? 0 : 1;
}

int
main(int argc, char **argv)
{
    mach_timebase_info(&g_tb);
    bool do_status = false, do_reset = false;
    int poke_cpu = -1, poke_secs = 5;
    const char *poke_level = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--status")) do_status = true;
        else if (!strcmp(argv[i], "--reset")) do_reset = true;
        else if (!strcmp(argv[i], "--poke") && i + 2 < argc) {
            poke_cpu = atoi(argv[++i]);
            poke_level = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') poke_secs = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--shadow")) g_cfg.apply = false;
        else if (!strcmp(argv[i], "--apply")) g_cfg.apply = true;
        else if (!strcmp(argv[i], "--no-hid")) g_cfg.hid = false;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) g_cfg.csv_path = argv[++i];
        else if (!strcmp(argv[i], "--hot-cpu") && i + 1 < argc) g_cfg.hot_cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--smt-stride") && i + 1 < argc) g_cfg.smt_stride = atoi(argv[++i]) ? 1 : 0;
        else {
            fprintf(stderr, "usage: ryzend [--status | --reset | --poke CPU low|nom|high [SECS] | --shadow | --apply] [--csv file] "
                            "[--hot-cpu N] [--smt-stride 0|1] [--no-hid]\n");
            return 2;
        }
    }
    if (do_status) return cmd_status();
    if (do_reset)  return cmd_reset();
    if (poke_level) return cmd_poke(poke_cpu, poke_level, poke_secs);

    g_apply_mode = g_cfg.apply;
    if (g_cfg.apply && geteuid() != 0) {
        fprintf(stderr, "ryzend: --apply needs root\n");
        return 1;
    }

    g_have_kext = kext_open();
    if (g_cfg.apply && !g_have_kext) return 1;
    if (g_have_kext && !topology_refresh()) {
        if (g_cfg.apply) return 1;
        g_have_kext = false;
    }
    if (!g_have_kext) {
        /* Shadow without the kext: made-up caps, only the decisions are meaningful. */
        fprintf(stderr, "ryzend: no kext; shadow mode with placeholder caps\n");
        memset(&g_topo, 0, sizeof g_topo);
        g_topo.core_count = 16; g_topo.lowest_perf = 20; g_topo.nominal_perf = 100; g_topo.highest_perf = 150;
    }
    if (g_cfg.apply) {
        if (!(g_topo.flags & RYZEN_FLAG_WRITE_ENABLED)) {
            fprintf(stderr, "ryzend: kext is read-only. Boot with ryzenplatform_write=1, or use --shadow.\n");
            kext_close();
            return 1;
        }
        if (!(g_topo.flags & RYZEN_FLAG_CPPC_ENABLED)) {
            fprintf(stderr, "ryzend: CPPC is not enabled on any CPU; nothing to drive.\n");
            kext_close();
            return 1;
        }
        printf("ryzend: APPLY mode on %u CPUs. Ctrl-C restores the firmware's settings.\n", g_topo.core_count);
    } else {
        printf("ryzend: SHADOW mode (nothing is written).\n");
    }

    csv_open();
    install_signal(SIGINT);
    install_signal(SIGTERM);
    if (g_cfg.hid) hid_install();
    power_install();

    pthread_t th;
    if (pthread_create(&th, NULL, policy_thread, NULL) != 0) {
        perror("ryzend: pthread_create");
        return 1;
    }

    CFRunLoopRun();

    atomic_store(&g_stop, 1);
    poke_policy();
    pthread_join(th, NULL);
    if (g_have_kext) {
        if (g_apply_mode) {
            kext_reset();        /* the last thing we do, whatever happened above */
        }
        kext_close();
    }
    if (g_csv) fclose(g_csv);
    return 0;
}
