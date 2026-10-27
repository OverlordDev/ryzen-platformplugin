/*
 * RyzenPlatform.cpp - v0.1 "eyes".
 *
 * What it does: every second, on every CPU, reads the CPPC MSRs, the current
 * P-state and the APERF/MPERF/TSC counters, reads Tctl from the SMU through
 * SMN, and publishes the result as IORegistry properties. It never writes an
 * MSR and never touches the SMU mailbox. The only write it makes at all is to
 * the SMN *index* register (host bridge config 0x60) so that the data register
 * (0x64) can be read; that is how SMN reads work and it changes no state.
 *
 * Why a kext at all: macOS on this box has no CPU temperature sensor and no
 * frequency control (XCPM is off, CPUPStates is empty). rdmsr and the SMN
 * index/data registers are the only way to see what the SMU is doing.
 *
 * Safety:
 *   - every MSR read goes through rdmsr_carefully(), which returns non-zero
 *     instead of taking a #GP when the MSR does not exist;
 *   - CPPC is only touched when CPUID Fn8000_0008 EBX[27] says it exists;
 *   - kill switch: boot-arg "ryzenplatform=0" or "-ryzenpmoff" keeps the kext
 *     from starting at all;
 *   - the SMN index/data pair is not atomic and other kexts (SMCAMDProcessor,
 *     AMDRyzenCPUPowerManagement) use the same pair with no shared lock. We
 *     serialise our own users with a lock and detect a foreign interleave by
 *     re-reading the index; see smnRead().
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <i386/proc_reg.h>
#include <libkern/libkern.h>
#include <stdint.h>

#include "ZenMsr.h"
#include "KernelDecls.hpp"
#include "RyzenPlatform.hpp"

#define kMaxCpus        64
#define kSamplePeriodMs 1000
#define kSmnAttempts    3

/* ---------------------------------------------------------------- globals */

struct CpuSnapshot {
    uint64_t cap1;
    uint64_t enable;
    uint64_t cap2;
    uint64_t req;
    uint64_t status;
    uint64_t pstate;
    uint64_t pstateDef;
    uint64_t aperf;
    uint64_t mperf;
    uint64_t tsc;
};

#define kNumArrays 10

static CpuSnapshot gSnap[kMaxCpus];
static uint32_t    gCpuCount;
static bool        gCppcSupported;
static bool        gAperfMperf;

/* Which CPU numbers showed up in the counting rendezvous. Each CPU writes only
 * its own byte, so there is no read-modify-write race between CPUs. */
static volatile uint8_t gSeen[kMaxCpus];

/* Scratch buffers for publishing (the timer is the only writer). */
static uint64_t gOut64[kNumArrays][kMaxCpus];

/* ------------------------------------------------------------------- cpuid */

static inline void
cpuid_leaf(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                             : "a"(leaf), "c"(0));
}

static inline uint64_t
readTsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool
probeAmdCppc(void)
{
    uint32_t a, b, c, d;

    cpuid_leaf(0, &a, &b, &c, &d);
    /* "AuthenticAMD" */
    if (!(b == 0x68747541u && d == 0x69746E65u && c == 0x444D4163u)) {
        IOLog("RyzenPlatform: not an AMD CPU, staying out\n");
        return false;
    }

    /* APERF/MPERF: CPUID Fn0000_0006 ECX[0]. Leaf 0 EAX is the highest basic leaf. */
    gAperfMperf = false;
    if (a >= 6) {
        uint32_t a6, b6, c6, d6;
        cpuid_leaf(6, &a6, &b6, &c6, &d6);
        gAperfMperf = (c6 & 1u) != 0;
    }

    cpuid_leaf(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000008u) {
        IOLog("RyzenPlatform: no CPUID Fn8000_0008, CPPC unknown\n");
        return false;
    }

    cpuid_leaf(0x80000008u, &a, &b, &c, &d);
    bool cppc = (b & (1u << 27)) != 0;
    bool pref = (b & (1u << 28)) != 0;
    IOLog("RyzenPlatform: CPUID Fn8000_0008 EBX=0x%08x cppc=%d preferred_cores=%d aperf_mperf=%d ncpu=%u\n",
          b, cppc ? 1 : 0, pref ? 1 : 0, gAperfMperf ? 1 : 0, (a & 0xFFu) + 1);
    return cppc;
}

/* Kill switch. Either spelling works so that docs and muscle memory cannot
 * disagree with the code: "ryzenplatform=0" or the flag "-ryzenpmoff". */
static bool
disabledByBootArg(void)
{
    int mode = 1;
    if (PE_parse_boot_argn("ryzenplatform", &mode, sizeof(mode)) && mode == 0) {
        IOLog("RyzenPlatform: disabled by boot-arg ryzenplatform=0\n");
        return true;
    }

    int flag = 0;
    if (PE_parse_boot_argn("-ryzenpmoff", &flag, sizeof(flag))) {
        IOLog("RyzenPlatform: disabled by boot-arg -ryzenpmoff\n");
        return true;
    }
    return false;
}

/* --------------------------------------------------------------- msr reads */

static inline bool
readMsr(uint32_t msr, uint64_t *value)
{
    uint32_t lo = 0, hi = 0;
    if (rdmsr_carefully(msr, &lo, &hi) != 0) {
        return false;
    }
    *value = ((uint64_t)hi << 32) | lo;
    return true;
}

static void
countOneCpu(void * /*arg*/)
{
    int cpu = cpu_number();
    if (cpu >= 0 && cpu < kMaxCpus) {
        gSeen[cpu] = 1;
    }
}

/* Runs the counting rendezvous and fills gCpuCount. Returns false unless the
 * CPU numbers are dense (0..n-1), which the rest of the code relies on. */
static bool
countCpus(void)
{
    bzero((void *)gSeen, sizeof(gSeen));
    mp_rendezvous_no_intrs(countOneCpu, NULL);

    uint32_t n = 0;
    while (n < kMaxCpus && gSeen[n]) {
        n++;
    }
    for (uint32_t i = n; i < kMaxCpus; i++) {
        if (gSeen[i]) {
            IOLog("RyzenPlatform: cpu numbers are sparse (%u seen after gap at %u)\n", i, n);
            return false;
        }
    }
    gCpuCount = n;
    return n != 0;
}

static void
sampleOneCpu(void * /*arg*/)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= (int)gCpuCount) {
        return;
    }

    CpuSnapshot *s = &gSnap[cpu];

    /* Counters first and back to back, so MPERF/APERF/TSC describe the same
     * instant as closely as we can manage with interrupts off. */
    s->aperf = s->mperf = ~0ull;
    if (gAperfMperf) {
        uint64_t m, a;
        if (readMsr(MSR_IA32_MPERF, &m) && readMsr(MSR_IA32_APERF, &a)) {
            s->mperf = m;
            s->aperf = a;
        }
    }
    s->tsc = readTsc();

    if (gCppcSupported) {
        if (!readMsr(MSR_AMD_CPPC_CAP1,   &s->cap1))   s->cap1   = ~0ull;
        if (!readMsr(MSR_AMD_CPPC_ENABLE, &s->enable)) s->enable = ~0ull;
        if (!readMsr(MSR_AMD_CPPC_CAP2,   &s->cap2))   s->cap2   = ~0ull;
        if (!readMsr(MSR_AMD_CPPC_REQ,    &s->req))    s->req    = ~0ull;
        if (!readMsr(MSR_AMD_CPPC_STATUS, &s->status)) s->status = ~0ull;
    }

    if (readMsr(MSR_AMD_PSTATE_STATUS, &s->pstate)) {
        uint32_t idx = (uint32_t)(s->pstate & 0x7u);
        if (!readMsr(MSR_AMD_PSTATE_DEF0 + idx, &s->pstateDef)) {
            s->pstateDef = ~0ull;
        }
    } else {
        s->pstate    = ~0ull;
        s->pstateDef = ~0ull;
    }
}

/* ---------------------------------------------------------------- SMN/Tctl */

static IOPCIDevice *gHostBridge;
static IOLock      *gSmnLock;
static uint32_t     gSmnRetries;   /* foreign index/data interleaves we caught */

static IOPCIDevice *
findHostBridge(void)
{
    OSIterator *iter = IOService::getMatchingServices(
        IOService::serviceMatching("IOPCIDevice"));
    if (!iter) {
        return NULL;
    }

    IOPCIDevice *found = NULL;
    IOService   *entry;

    while ((entry = (IOService *)iter->getNextObject())) {
        IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, entry);
        if (!pci) {
            continue;
        }
        if (pci->getBusNumber() == 0 && pci->getDeviceNumber() == 0 &&
            pci->getFunctionNumber() == 0) {
            found = pci;
            found->retain();
            break;
        }
    }
    iter->release();
    return found;
}

/*
 * SMN read through the host bridge: write the address to the index register
 * (0x60), read the value from the data register (0x64). The pair is stateful,
 * so two users interleaving corrupt each other.
 *
 * gSmnLock only orders *our own* callers (today the timer; later the user
 * client and the thermal guard). It cannot stop another kext. To catch that
 * case we re-read the index register after the data read: if it no longer
 * holds our address, somebody moved it under us, the value is not ours, and we
 * retry. gSmnRetries counts how often that happened; it is published so a
 * conflict with SMCAMDProcessor shows up in the data instead of as a rare
 * wrong temperature.
 */
static bool
smnRead(uint32_t address, uint32_t *value)
{
    if (!gSmnLock) {
        return false;
    }

    if (!gHostBridge) {
        gHostBridge = findHostBridge();
        if (!gHostBridge) {
            return false;
        }
        IOLog("RyzenPlatform: SMN host bridge %s\n",
              gHostBridge->getName());
    }

    bool ok = false;

    IOLockLock(gSmnLock);
    for (int attempt = 0; attempt < kSmnAttempts; attempt++) {
        gHostBridge->configWrite32((UInt8)0x60, address);
        uint32_t v = gHostBridge->configRead32((UInt8)0x64);
        if (gHostBridge->configRead32((UInt8)0x60) == address) {
            *value = v;
            ok = true;
            break;
        }
        gSmnRetries++;
    }
    IOLockUnlock(gSmnLock);

    return ok;
}

/* ---------------------------------------------------------------- publish  */

static void
publishArray(IOService *self, const char *key, const uint64_t *src)
{
    OSData *data = OSData::withBytes(src, gCpuCount * sizeof(uint64_t));
    if (data) {
        self->setProperty(key, data);
        data->release();
    }
}

/* ------------------------------------------------------------------ driver */

#define super IOService
OSDefineMetaClassAndStructors(com_illia_RyzenPlatform, IOService)

bool
com_illia_RyzenPlatform::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    if (disabledByBootArg()) {
        return false;
    }

    gCppcSupported = probeAmdCppc();
    gCpuCount      = 0;
    bzero(gSnap, sizeof(gSnap));

    /* One rendezvous just to learn how many CPUs we actually run on. */
    if (!countCpus() || gCpuCount > kMaxCpus) {
        IOLog("RyzenPlatform: bad cpu count %u, giving up\n", gCpuCount);
        return false;
    }

    gSmnLock    = IOLockAlloc();
    gSmnRetries = 0;
    if (!gSmnLock) {
        return false;
    }

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        teardown();
        return false;
    }

    fTimer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                   &com_illia_RyzenPlatform::onTimer));
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
        teardown();
        return false;
    }

    setProperty("kext.version", "0.1.0");
    setProperty("cppc.supported",
                gCppcSupported ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("aperf.supported",
                gAperfMperf ? kOSBooleanTrue : kOSBooleanFalse);

    sample();
    fTimer->setTimeoutMS(kSamplePeriodMs);

    registerService();
    IOLog("RyzenPlatform: started, %u cpus, cppc=%d\n", gCpuCount,
          gCppcSupported ? 1 : 0);
    return true;
}

/* Releases everything start() may have set up; safe to call on a half-built
 * instance, since IOKit does not call stop() when start() fails. */
void
com_illia_RyzenPlatform::teardown()
{
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fTimer);
        }
        fTimer->release();
        fTimer = NULL;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    if (gHostBridge) {
        gHostBridge->release();
        gHostBridge = NULL;
    }
    if (gSmnLock) {
        IOLockFree(gSmnLock);
        gSmnLock = NULL;
    }
}

void
com_illia_RyzenPlatform::stop(IOService *provider)
{
    teardown();
    super::stop(provider);
}

void
com_illia_RyzenPlatform::free()
{
    super::free();
}

void
com_illia_RyzenPlatform::onTimer()
{
    sample();
    if (fTimer) {
        fTimer->setTimeoutMS(kSamplePeriodMs);
    }
}

void
com_illia_RyzenPlatform::sample()
{
    mp_rendezvous_no_intrs(sampleOneCpu, NULL);

    for (uint32_t i = 0; i < gCpuCount; i++) {
        gOut64[0][i] = gSnap[i].cap1;
        gOut64[1][i] = gSnap[i].enable;
        gOut64[2][i] = gSnap[i].cap2;
        gOut64[3][i] = gSnap[i].req;
        gOut64[4][i] = gSnap[i].status;
        gOut64[5][i] = gSnap[i].pstate;
        gOut64[6][i] = gSnap[i].pstateDef;
        gOut64[7][i] = gSnap[i].aperf;
        gOut64[8][i] = gSnap[i].mperf;
        gOut64[9][i] = gSnap[i].tsc;
    }

    publishArray(this, "cppc.cap1",   gOut64[0]);
    publishArray(this, "cppc.enable", gOut64[1]);
    publishArray(this, "cppc.cap2",   gOut64[2]);
    publishArray(this, "cppc.req",    gOut64[3]);
    publishArray(this, "cppc.status", gOut64[4]);
    publishArray(this, "pstate",      gOut64[5]);
    publishArray(this, "pstate.def",  gOut64[6]);
    publishArray(this, "aperf",       gOut64[7]);
    publishArray(this, "mperf",       gOut64[8]);
    publishArray(this, "tsc",         gOut64[9]);

    uint32_t enabled = 0;
    for (uint32_t i = 0; i < gCpuCount; i++) {
        if ((gSnap[i].status & CPPC_STATUS_ENABLED) != 0) {
            enabled++;
        }
    }
    setProperty("cppc.enabled", (unsigned long long)enabled, 32);
    setProperty("cpu.count", (unsigned long long)gCpuCount, 32);

    /* temp.tctl is signed millidegrees (see smn_tctl_mdeg); temp.valid says
     * whether it means anything. An all-ones read is a dead bus, not 255 C. */
    uint32_t raw = 0;
    bool valid = smnRead(SMN_THM_TCON_CUR_TMP, &raw) && raw != 0xFFFFFFFFu;
    setProperty("temp.valid", valid ? kOSBooleanTrue : kOSBooleanFalse);
    if (valid) {
        int32_t mdeg = smn_tctl_mdeg(raw);
        setProperty("temp.tctl", (unsigned long long)(uint32_t)mdeg, 32);
        setProperty("temp.raw", (unsigned long long)raw, 32);
        setProperty("temp.range49",
                    (raw & SMN_TCTL_RANGE_SEL) ? kOSBooleanTrue : kOSBooleanFalse);
    } else {
        /* Do not leave the last good value looking current. */
        removeProperty("temp.tctl");
        removeProperty("temp.raw");
        removeProperty("temp.range49");
    }
    setProperty("smn.retries", (unsigned long long)gSmnRetries, 32);
}
