/*
 * Actuator.cpp - CPPC_REQ writer. See Actuator.hpp for the contract.
 *
 * Lock order: gLock (actuator state) -> gRvLock (rendezvous) -> gSmnLock (SMN,
 * inside ryzenReadTctl). The sampler only takes gRvLock and gSmnLock, never
 * gLock, so there is no cycle.
 *
 * Everything that runs inside a rendezvous (captureOne, writeOne) executes on
 * every CPU at once with interrupts off: no locks, no allocation, no logging,
 * and each CPU touches only its own gAct[] slot.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include <string.h>

#include "ZenMsr.h"
#include "KernelDecls.hpp"
#include "MsrIo.hpp"
#include "Actuator.hpp"
#include "RyzenPlatform.hpp"

#define kMaxCpus            64
#define kWatchdogNs         150000000ull    /* 3 missed 50 ms heartbeats */
#define kThermalPeriodNs    250000000ull

#define kGuardCapMdeg       90000           /* >= : cap at nominal            */
#define kGuardHardMdeg      98000           /* >= : everything at lowest      */
#define kGuardKillMdeg      100000          /* >= : restore firmware, lockout */
#define kGuardReleaseMdeg   85000           /* hysteresis for leaving a cap   */

struct CpuAct {
    uint64_t origReq;       /* firmware's CPPC_REQ when we armed; what "auto" means */
    uint64_t lastWritten;   /* what we believe is in CPPC_REQ now (origReq if untouched) */
    uint64_t wantReq;       /* the daemon's request, clamped to caps, before the guard */
    uint64_t expiryNs;      /* 0 = no TTL */
    uint8_t  lowest, nominal, highest;
    bool     wantValid;

    /* Scratch shared with the rendezvous callbacks. */
    uint64_t writeReq;
    uint64_t readBack;
    uint64_t cap1, enable, status, reqNow;
    bool     doWrite, wrOk, vfOk, capRd;
};

static CpuAct   gAct[kMaxCpus];
static uint32_t gN;
static bool     gCppc;
static bool     gWriteEnabled;

static IOLock  *gLock;
static IOLock  *gRvLock;

static bool     gArmed;          /* origReq captured and caps validated */
static bool     gOverride;       /* at least one CPU may differ from firmware */
static bool     gLockout;        /* latched safety stop */
static bool     gSleeping;
static uint32_t gGuard;
static bool     gTempValid;
static int32_t  gTempMdeg;
static uint64_t gLastBeatNs;
static uint64_t gLastThermalNs;
static uint32_t gWatchdogResets;
static uint32_t gReadbackFailures;
static ryzen_error_t gLastErr;

/* --------------------------------------------------------------- helpers */

static uint64_t
nowNs(void)
{
    uint64_t abs = 0, ns = 0;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}

static void
setErr(uint32_t code, uint32_t core, const char *msg)
{
    bzero(&gLastErr, sizeof(gLastErr));
    gLastErr.abi_version  = RYZEN_PLATFORM_ABI_VERSION;
    gLastErr.struct_size  = sizeof(gLastErr);
    gLastErr.code         = code;
    gLastErr.core         = core;
    gLastErr.timestamp_ns = nowNs();
    strlcpy(gLastErr.message, msg, sizeof(gLastErr.message));
    IOLog("RyzenPlatform: error %u cpu %u: %s\n", code, core, msg);
}

static inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Keeps the reserved upper half of the MSR exactly as the firmware had it. */
static uint64_t
packReq(uint64_t base, uint32_t max, uint32_t min, uint32_t des, uint32_t epp)
{
    return (base & ~0xFFFFFFFFull) | (uint64_t)(max & 0xFF) | ((uint64_t)(min & 0xFF) << 8) |
           ((uint64_t)(des & 0xFF) << 16) | ((uint64_t)(epp & 0xFF) << 24);
}

/* wantReq as the CPU should actually see it, given the thermal guard. */
static uint64_t
guardedReq(const CpuAct *a)
{
    uint32_t max = CPPC_REQ_MAX(a->wantReq);
    uint32_t min = CPPC_REQ_MIN(a->wantReq);
    uint32_t des = CPPC_REQ_DESIRED(a->wantReq);
    uint32_t epp = CPPC_REQ_EPP(a->wantReq);

    if (gGuard >= 2) {
        max = min = des = a->lowest;
        epp = 255;
    } else if (gGuard == 1) {
        if (max > a->nominal) max = a->nominal;
        if (min > max)        min = max;
        if (des > max)        des = max;
        epp = 255;
    }
    return packReq(a->origReq, max, min, des, epp);
}

static uint32_t
guardLevelFor(bool ok, int32_t mdeg, uint32_t cur)
{
    if (!ok) {
        return cur > 1 ? cur : 1;          /* no data is not "fine" */
    }
    if (mdeg >= kGuardKillMdeg) return 3;
    if (mdeg >= kGuardHardMdeg) return 2;
    if (mdeg >= kGuardCapMdeg)  return 1;
    if (cur > 0 && mdeg > kGuardReleaseMdeg) {
        return 1;                          /* hold a cap until clearly cooled */
    }
    return 0;
}

/* ----------------------------------------------------- rendezvous actions */

void
ryzenRendezvous(void (*fn)(void *))
{
    if (gRvLock) IOLockLock(gRvLock);
    mp_rendezvous_no_intrs(fn, NULL);
    if (gRvLock) IOLockUnlock(gRvLock);
}

static void
captureOne(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= (int)gN) {
        return;
    }
    CpuAct *a = &gAct[cpu];
    a->capRd = readMsr(MSR_AMD_CPPC_CAP1,   &a->cap1)   &&
               readMsr(MSR_AMD_CPPC_ENABLE, &a->enable) &&
               readMsr(MSR_AMD_CPPC_STATUS, &a->status) &&
               readMsr(MSR_AMD_CPPC_REQ,    &a->reqNow);
}

static void
writeOne(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= (int)gN) {
        return;
    }
    CpuAct *a = &gAct[cpu];
    a->wrOk = a->vfOk = false;
    if (!a->doWrite) {
        return;
    }
    if (writeMsr(MSR_AMD_CPPC_REQ, a->writeReq)) {
        a->wrOk = true;
        uint64_t rb = 0;
        if (readMsr(MSR_AMD_CPPC_REQ, &rb)) {
            a->readBack = rb;
            /* Compare the architected low half; the upper half is reserved. */
            a->vfOk = ((uint32_t)rb == (uint32_t)a->writeReq);
        }
    }
}

/* ------------------------------------------------------ locked internals */

/* Returns RYZEN_OK when every CPU is usable and origReq is captured. */
static int
armLocked(void)
{
    if (gArmed) {
        return RYZEN_OK;
    }
    if (!gCppc || gN == 0) {
        setErr(RYZEN_E_UNSUPPORTED, 0, "CPPC not advertised by CPUID");
        return RYZEN_E_UNSUPPORTED;
    }

    ryzenRendezvous(captureOne);

    for (uint32_t i = 0; i < gN; i++) {
        CpuAct *a = &gAct[i];
        uint32_t lo = CPPC_CAP1_LOWEST(a->cap1);
        uint32_t nm = CPPC_CAP1_NOMINAL(a->cap1);
        uint32_t hi = CPPC_CAP1_HIGHEST(a->cap1);

        if (!a->capRd) {
            setErr(RYZEN_E_UNSUPPORTED, i, "CPPC MSR read failed");
            return RYZEN_E_UNSUPPORTED;
        }
        /* ENABLE bit 0 is the architected switch. We never set it ourselves:
         * if the firmware left it off, hand control back rather than guess. */
        if ((a->enable & 1u) == 0) {
            setErr(RYZEN_E_UNSUPPORTED, i, "CPPC_ENABLE bit 0 is clear; not enabling it blindly");
            return RYZEN_E_UNSUPPORTED;
        }
        if (hi == 0 || lo == 0 || lo > nm || nm > hi) {
            setErr(RYZEN_E_UNSUPPORTED, i, "CPPC caps look invalid");
            return RYZEN_E_UNSUPPORTED;
        }
        a->lowest = (uint8_t)lo;
        a->nominal = (uint8_t)nm;
        a->highest = (uint8_t)hi;
    }
    for (uint32_t i = 0; i < gN; i++) {
        CpuAct *a = &gAct[i];
        a->origReq     = a->reqNow;
        a->lastWritten = a->reqNow;
        a->wantReq     = a->reqNow;
        a->wantValid   = false;
        a->expiryNs    = 0;
    }
    gArmed = true;
    return RYZEN_OK;
}

/* Puts every CPU back to the firmware's value. Never fails loudly enough to
 * block a caller: a restore that does not stick latches the lockout instead. */
static void
restoreAllLocked(const char *why)
{
    bool any = false;
    for (uint32_t i = 0; i < gN; i++) {
        CpuAct *a = &gAct[i];
        a->wantValid = false;
        a->expiryNs  = 0;
        a->doWrite   = false;
        if (gArmed && a->lastWritten != a->origReq) {
            a->writeReq = a->origReq;
            a->doWrite  = true;
            any = true;
        }
    }
    if (any) {
        ryzenRendezvous(writeOne);
        for (uint32_t i = 0; i < gN; i++) {
            CpuAct *a = &gAct[i];
            if (!a->doWrite) continue;
            if (a->wrOk && a->vfOk) {
                a->lastWritten = a->origReq;
            } else {
                gLockout = true;
                setErr(RYZEN_E_READBACK, i, "restore of firmware CPPC_REQ did not stick");
            }
        }
    }
    gOverride = false;
    gArmed    = false;
    IOLog("RyzenPlatform: back to firmware CPPC_REQ (%s)\n", why);
}

/* Writes whatever differs between what each CPU should have and what it has. */
static int
commitLocked(void)
{
    bool any = false, anyWant = false;

    for (uint32_t i = 0; i < gN; i++) {
        CpuAct *a = &gAct[i];
        uint64_t target = a->wantValid ? guardedReq(a) : a->origReq;
        a->doWrite = false;
        if (a->wantValid) anyWant = true;
        if (target != a->lastWritten) {
            a->writeReq = target;
            a->doWrite  = true;
            any = true;
        }
    }

    int rc = RYZEN_OK;
    if (any) {
        ryzenRendezvous(writeOne);
        for (uint32_t i = 0; i < gN; i++) {
            CpuAct *a = &gAct[i];
            if (!a->doWrite) continue;
            if (a->wrOk && a->vfOk) {
                a->lastWritten = a->writeReq;
            } else {
                gReadbackFailures++;
                setErr(RYZEN_E_READBACK, i,
                       a->wrOk ? "CPPC_REQ read-back differs from what was written"
                               : "wrmsr to CPPC_REQ refused");
                rc = RYZEN_E_READBACK;
            }
        }
    }

    if (rc != RYZEN_OK) {
        restoreAllLocked("write failed");
        gLockout = true;
        return rc;
    }
    gOverride = anyWant;
    if (!anyWant) {
        gArmed = false;        /* nothing of ours left in hardware; recapture next time */
    }
    return RYZEN_OK;
}

/* ------------------------------------------------------------- lifecycle */

bool
actInit(bool cppcSupported, bool writeEnabled)
{
    gLock   = IOLockAlloc();
    gRvLock = IOLockAlloc();
    if (!gLock || !gRvLock) {
        actShutdown();
        return false;
    }
    bzero(gAct, sizeof(gAct));
    bzero(&gLastErr, sizeof(gLastErr));
    gN = 0;
    gCppc = cppcSupported;
    gWriteEnabled = writeEnabled;
    gArmed = gOverride = gLockout = gSleeping = false;
    gGuard = 0;
    gTempValid = false;
    gTempMdeg = 0;
    gWatchdogResets = gReadbackFailures = 0;
    gLastBeatNs = gLastThermalNs = 0;
    return true;
}

void
actSetCpuCount(uint32_t n)
{
    gN = n > kMaxCpus ? kMaxCpus : n;
}

void
actShutdown(void)
{
    if (gLock) {
        IOLockLock(gLock);
        restoreAllLocked("shutdown");
        IOLockUnlock(gLock);
    }
    /* The lock pointers are cleared before freeing so a late rendezvous caller
     * (the sampler) sees NULL and runs unlocked rather than using freed memory. */
    IOLock *l1 = gLock, *l2 = gRvLock;
    gLock = NULL;
    gRvLock = NULL;
    if (l1) IOLockFree(l1);
    if (l2) IOLockFree(l2);
}

/* ------------------------------------------------------------- queries */

int
actTopology(ryzen_topology_t *out)
{
    bzero(out, sizeof(*out));
    out->abi_version = RYZEN_PLATFORM_ABI_VERSION;
    out->struct_size = sizeof(*out);
    out->core_count  = gN;
    out->cppc_supported = gCppc ? 1 : 0;

    uint32_t enabled = 0, best = 0, bestHi = 0;
    bool have = false;
    for (uint32_t i = 0; i < gN; i++) {
        RyzenCpuSample s;
        if (!ryzenGetSample(i, &s)) continue;
        if (s.enable != ~0ull && (s.enable & 1u)) enabled++;
        if (s.cap1 != ~0ull) {
            uint32_t hi = CPPC_CAP1_HIGHEST(s.cap1);
            if (!have || hi > bestHi) { bestHi = hi; best = i; have = true; }
        }
    }
    out->cppc_enabled = enabled;

    if (have) {
        RyzenCpuSample s;
        if (ryzenGetSample(best, &s)) {
            out->preferred_core = best;      /* the core the SMU rates highest */
            out->highest_perf   = CPPC_CAP1_HIGHEST(s.cap1);
            out->nominal_perf   = CPPC_CAP1_NOMINAL(s.cap1);
            out->lowest_perf    = CPPC_CAP1_LOWEST(s.cap1);
        }
    }

    /* Frequencies: P0's FID/DID is the one number the hardware states outright.
     * The rest is the linear perf->MHz scaling Linux amd-pstate assumes. It is an
     * ESTIMATE; treat it as informational, never as an input to a decision. */
    RyzenCpuSample s0;
    if (ryzenGetSample(0, &s0) && s0.pstateDef != ~0ull) {
        uint32_t fid = PSTATE_DEF_FID(s0.pstateDef), did = PSTATE_DEF_DID(s0.pstateDef);
        if (did != 0 && out->nominal_perf != 0) {
            uint32_t mhz = 200u * fid / did;
            out->nominal_freq_mhz = mhz;
            out->highest_freq_mhz = mhz * out->highest_perf / out->nominal_perf;
            out->lowest_freq_mhz  = mhz * out->lowest_perf  / out->nominal_perf;
        }
    }

    if (gLock) IOLockLock(gLock);
    if (gWriteEnabled) out->flags |= RYZEN_FLAG_WRITE_ENABLED;
    if (gOverride)     out->flags |= RYZEN_FLAG_OVERRIDE_ACTIVE | RYZEN_FLAG_WATCHDOG_ARMED;
    if (gGuard)        out->flags |= RYZEN_FLAG_THERMAL_GUARD;
    if (gLockout)      out->flags |= RYZEN_FLAG_LOCKOUT;
    if (gSleeping)     out->flags |= RYZEN_FLAG_SLEEPING;
    if (gLock) IOLockUnlock(gLock);
    if (gCppc)         out->flags |= RYZEN_FLAG_CPPC_SUPPORTED;
    if (enabled)       out->flags |= RYZEN_FLAG_CPPC_ENABLED;
    return RYZEN_OK;
}

int
actReadState(uint32_t cpu, ryzen_core_state_t *out)
{
    if (cpu >= gN) {
        return RYZEN_E_RANGE;
    }
    bzero(out, sizeof(*out));
    out->abi_version = RYZEN_PLATFORM_ABI_VERSION;
    out->struct_size = sizeof(*out);
    out->core        = cpu;

    /* What the hardware holds: our last write if we have one, else the 1 Hz sample. */
    uint64_t req = 0;
    bool haveReq = false;
    if (gLock) IOLockLock(gLock);
    if (gArmed) {
        req = gAct[cpu].lastWritten;
        haveReq = true;
        if (gOverride) out->flags |= RYZEN_FLAG_OVERRIDE_ACTIVE;
    }
    if (gLock) IOLockUnlock(gLock);
    if (!haveReq) {
        RyzenCpuSample s;
        if (ryzenGetSample(cpu, &s) && s.req != ~0ull) {
            req = s.req;
            haveReq = true;
        }
    }
    if (haveReq) {
        out->max_perf     = CPPC_REQ_MAX(req);
        out->min_perf     = CPPC_REQ_MIN(req);
        out->desired_perf = CPPC_REQ_DESIRED(req);
        out->epp          = CPPC_REQ_EPP(req);
    }

    int32_t t = 0;
    out->temp_millic = ryzenReadTctl(&t) ? (uint32_t)t : 0xFFFFFFFFu;
    /* effective_freq_mhz and package_power_mw are not provided by the kext: use the
     * aperf/mperf/tsc arrays in the IORegistry (ryzenstat does the delta math). */
    return RYZEN_OK;
}

void
actStatus(ryzen_status_t *out)
{
    bzero(out, sizeof(*out));
    out->abi_version = RYZEN_PLATFORM_ABI_VERSION;
    out->struct_size = sizeof(*out);
    out->cpu_count   = gN;
    out->smn_retries = ryzenSmnRetries();
    out->tctl_millic = RYZEN_TEMP_UNKNOWN;

    if (gLock) IOLockLock(gLock);
    if (gWriteEnabled) out->flags |= RYZEN_FLAG_WRITE_ENABLED;
    if (gCppc)         out->flags |= RYZEN_FLAG_CPPC_SUPPORTED;
    if (gOverride)     out->flags |= RYZEN_FLAG_OVERRIDE_ACTIVE | RYZEN_FLAG_WATCHDOG_ARMED;
    if (gGuard)        out->flags |= RYZEN_FLAG_THERMAL_GUARD;
    if (gLockout)      out->flags |= RYZEN_FLAG_LOCKOUT;
    if (gSleeping)     out->flags |= RYZEN_FLAG_SLEEPING;
    out->guard_level       = gGuard;
    out->watchdog_resets   = gWatchdogResets;
    out->readback_failures = gReadbackFailures;
    if (gOverride) {
        uint64_t age = nowNs() - gLastBeatNs;
        out->heartbeat_age_ms = (uint32_t)(age / 1000000ull);
    }
    if (gLock) IOLockUnlock(gLock);

    int32_t t = 0;
    if (ryzenReadTctl(&t)) {
        out->tctl_millic = t;
    }
}

void
actLastError(ryzen_error_t *out)
{
    if (gLock) IOLockLock(gLock);
    *out = gLastErr;
    if (gLock) IOLockUnlock(gLock);
    out->abi_version = RYZEN_PLATFORM_ABI_VERSION;
    out->struct_size = sizeof(*out);
}

bool
actNeedsTick(void)
{
    return gOverride;
}

/* ------------------------------------------------------------- commands */

int
actApply(const ryzen_apply_t *in)
{
    if (!ryzen_abi_compatible(in->abi_version) || in->struct_size != sizeof(*in)) {
        return RYZEN_E_ABI;
    }
    if (!gLock) {
        return RYZEN_E_INTERNAL;
    }

    IOLockLock(gLock);
    int rc = RYZEN_OK;

    if (!gWriteEnabled) {
        setErr(RYZEN_E_DISABLED, 0, "writes are off; boot with ryzenplatform_write=1");
        rc = RYZEN_E_DISABLED;
    } else if (gLockout) {
        setErr(RYZEN_E_LOCKOUT, 0, "safety lockout latched; fix the cause, then resetAuto");
        rc = RYZEN_E_LOCKOUT;
    } else if (gSleeping) {
        setErr(RYZEN_E_INTERNAL, 0, "system is going to sleep");
        rc = RYZEN_E_INTERNAL;
    } else if (in->count > RYZEN_PLATFORM_MAX_CORES) {
        setErr(RYZEN_E_RANGE, 0, "too many requests");
        rc = RYZEN_E_RANGE;
    } else {
        for (uint32_t i = 0; i < in->count; i++) {
            if (in->req[i].core >= gN) {
                setErr(RYZEN_E_RANGE, in->req[i].core, "cpu index out of range");
                rc = RYZEN_E_RANGE;
                break;
            }
        }
    }

    if (rc == RYZEN_OK) {
        rc = armLocked();
    }

    if (rc == RYZEN_OK) {
        uint64_t now = nowNs();
        for (uint32_t i = 0; i < in->count; i++) {
            const ryzen_perf_request_t *r = &in->req[i];
            CpuAct *a = &gAct[r->core];

            /* 0 means "not specified": max -> highest, min -> lowest. desired 0 stays
             * 0 (the SMU picks inside [min, max]). Everything is clamped to this CPU. */
            uint32_t max = r->max_perf ? clampU32(r->max_perf, a->lowest, a->highest) : a->highest;
            uint32_t min = r->min_perf ? clampU32(r->min_perf, a->lowest, a->highest) : a->lowest;
            if (min > max) min = max;
            uint32_t des = r->desired_perf ? clampU32(r->desired_perf, min, max) : 0;
            uint32_t epp = r->epp > 255 ? 255 : r->epp;

            a->wantReq   = packReq(a->origReq, max, min, des, epp);
            a->wantValid = true;
            a->expiryNs  = r->ttl_ms ? now + (uint64_t)r->ttl_ms * 1000000ull : 0;
        }
        gOverride   = true;
        gLastBeatNs = now;          /* grace: the first heartbeat may trail the first apply */
        rc = commitLocked();
    }
    IOLockUnlock(gLock);
    return rc;
}

int
actResetAuto(const char *why, bool userRequested)
{
    if (!gLock) {
        return RYZEN_OK;
    }
    IOLockLock(gLock);
    restoreAllLocked(why);

    if (userRequested && gLockout) {
        /* Only clear the latch when the temperature is back in range. */
        int32_t t = 0;
        bool ok = ryzenReadTctl(&t);
        uint32_t lvl = guardLevelFor(ok, t, 0);
        if (ok && lvl < 3) {
            gLockout = false;
            gGuard   = lvl;
            IOLog("RyzenPlatform: lockout cleared\n");
        } else {
            setErr(RYZEN_E_LOCKOUT, 0, "lockout stays: temperature unknown or too high");
        }
    }
    int rc = gLockout ? RYZEN_E_LOCKOUT : RYZEN_OK;
    IOLockUnlock(gLock);
    return rc;
}

int
actHeartbeat(const ryzen_heartbeat_t *hb)
{
    if (!ryzen_abi_compatible(hb->abi_version) || hb->struct_size != sizeof(*hb)) {
        return RYZEN_E_ABI;
    }
    if (!gLock) {
        return RYZEN_E_INTERNAL;
    }
    IOLockLock(gLock);
    gLastBeatNs = nowNs();
    IOLockUnlock(gLock);
    return RYZEN_OK;
}

/* ----------------------------------------------------------------- tick */

void
actTick(void)
{
    if (!gLock) {
        return;
    }
    IOLockLock(gLock);
    if (!gOverride) {
        IOLockUnlock(gLock);
        return;
    }

    uint64_t now = nowNs();

    /* 1. Watchdog. The daemon is the only thing that keeps an override alive. */
    if (now - gLastBeatNs > kWatchdogNs) {
        gWatchdogResets++;
        setErr(RYZEN_E_WATCHDOG, 0, "no heartbeat for 150 ms; restored firmware CPPC_REQ");
        restoreAllLocked("watchdog");
        IOLockUnlock(gLock);
        return;
    }

    /* 2. Per-CPU TTL: an expired request reverts that CPU to the firmware value. */
    bool expired = false;
    for (uint32_t i = 0; i < gN; i++) {
        CpuAct *a = &gAct[i];
        if (a->wantValid && a->expiryNs && now >= a->expiryNs) {
            a->wantValid = false;
            a->expiryNs  = 0;
            expired = true;
        }
    }
    if (expired) {
        commitLocked();
        if (!gOverride) {
            IOLockUnlock(gLock);
            return;
        }
    }

    /* 3. Thermal guard, independent of anything the daemon thinks. */
    if (now - gLastThermalNs >= kThermalPeriodNs) {
        gLastThermalNs = now;
        int32_t t = 0;
        bool ok = ryzenReadTctl(&t);
        gTempValid = ok;
        gTempMdeg  = ok ? t : 0;

        uint32_t lvl = guardLevelFor(ok, t, gGuard);
        if (lvl != gGuard) {
            IOLog("RyzenPlatform: thermal guard %u -> %u (%s, %d mC)\n", gGuard, lvl,
                  ok ? "sensor" : "NO SENSOR", ok ? t : 0);
            gGuard = lvl;
            if (lvl >= 3) {
                setErr(RYZEN_E_THERMAL, 0, "Tctl >= 100 C; restored firmware CPPC_REQ");
                restoreAllLocked("thermal kill");
                gLockout = true;
            } else {
                commitLocked();
            }
        }
    }
    IOLockUnlock(gLock);
}

/* ---------------------------------------------------------------- sleep */

void
actPrepareForSleep(void)
{
    if (!gLock) return;
    IOLockLock(gLock);
    gSleeping = true;
    restoreAllLocked("going to sleep");
    IOLockUnlock(gLock);
}

void
actWake(void)
{
    if (!gLock) return;
    IOLockLock(gLock);
    gSleeping = false;
    gArmed    = false;          /* firmware may have changed CPPC state across sleep: recapture */
    IOLockUnlock(gLock);
}
