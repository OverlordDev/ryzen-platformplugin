/*
 * RyzenPlatform.cpp - v0.1 "eyes".
 *
 * What it does: every second, on every CPU, reads the CPPC MSRs and the
 * current P-state, reads Tctl from the SMU through SMN, and publishes the
 * result as IORegistry properties. Nothing is ever written: no wrmsr, no SMN
 * writes, no SMU mailbox.
 *
 * Why a kext at all: macOS on this box has no CPU temperature sensor and no
 * frequency control (XCPM is off, CPUPStates is empty). rdmsr and the SMN
 * index/data registers are the only way to see what the SMU is doing.
 *
 * Safety:
 *   - every MSR read goes through rdmsr_carefully(), which returns non-zero
 *     instead of taking a #GP when the MSR does not exist;
 *   - CPPC is only touched when CPUID Fn8000_0008 EBX[27] says it exists;
 *   - boot-arg "ryzenplatform=0" keeps the kext from starting at all.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
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

/* ---------------------------------------------------------------- globals */

struct CpuSnapshot {
    uint64_t cap1;
    uint64_t enable;
    uint64_t cap2;
    uint64_t req;
    uint64_t status;
    uint64_t pstate;
    uint64_t pstateDef;
};

static CpuSnapshot gSnap[kMaxCpus];
static uint32_t    gCpuCount;
static bool        gCppcSupported;

/* Scratch buffers for publishing (the timer is the only writer). */
static uint64_t gOut64[7][kMaxCpus];

/* ------------------------------------------------------------------- cpuid */

static inline void
cpuid_leaf(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                             : "a"(leaf), "c"(0));
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

    cpuid_leaf(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000008u) {
        IOLog("RyzenPlatform: no CPUID Fn8000_0008, CPPC unknown\n");
        return false;
    }

    cpuid_leaf(0x80000008u, &a, &b, &c, &d);
    bool cppc = (b & (1u << 27)) != 0;
    bool pref = (b & (1u << 28)) != 0;
    IOLog("RyzenPlatform: CPUID Fn8000_0008 EBX=0x%08x cppc=%d preferred_cores=%d ncpu=%u\n",
          b, cppc ? 1 : 0, pref ? 1 : 0, (a & 0xFFu) + 1);
    return cppc;
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
    if (cpu >= 0 && cpu < kMaxCpus && (uint32_t)(cpu + 1) > gCpuCount) {
        gCpuCount = (uint32_t)(cpu + 1);
    }
}

static void
sampleOneCpu(void * /*arg*/)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= (int)gCpuCount) {
        return;
    }

    CpuSnapshot *s = &gSnap[cpu];

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

static bool
smnRead(uint32_t address, uint32_t *value)
{
    if (!gHostBridge) {
        gHostBridge = findHostBridge();
        if (!gHostBridge) {
            return false;
        }
        IOLog("RyzenPlatform: SMN host bridge %s\n",
              gHostBridge->getName());
    }

    gHostBridge->configWrite32((UInt8)0x60, address);
    *value = gHostBridge->configRead32((UInt8)0x64);
    return true;
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

    int mode = 1;
    if (PE_parse_boot_argn("ryzenplatform", &mode, sizeof(mode)) && mode == 0) {
        IOLog("RyzenPlatform: disabled by boot-arg ryzenplatform=0\n");
        return false;
    }

    gCppcSupported = probeAmdCppc();
    gCpuCount      = 0;
    bzero(gSnap, sizeof(gSnap));

    /* One rendezvous just to learn how many CPUs we actually run on. */
    mp_rendezvous_no_intrs(countOneCpu, NULL);
    if (gCpuCount == 0 || gCpuCount > kMaxCpus) {
        IOLog("RyzenPlatform: bad cpu count %u, giving up\n", gCpuCount);
        return false;
    }

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        return false;
    }

    fTimer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                   &com_illia_RyzenPlatform::onTimer));
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
        return false;
    }

    setProperty("kext.version", "0.1.0");
    setProperty("cppc.supported",
                gCppcSupported ? kOSBooleanTrue : kOSBooleanFalse);

    sample();
    fTimer->setTimeoutMS(kSamplePeriodMs);

    registerService();
    IOLog("RyzenPlatform: started, %u cpus, cppc=%d\n", gCpuCount,
          gCppcSupported ? 1 : 0);
    return true;
}

void
com_illia_RyzenPlatform::stop(IOService *provider)
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

    /* cpu_number() is dense, but be defensive about a sparse first sample. */
    for (uint32_t i = 0; i < gCpuCount; i++) {
        gOut64[0][i] = gSnap[i].cap1;
        gOut64[1][i] = gSnap[i].enable;
        gOut64[2][i] = gSnap[i].cap2;
        gOut64[3][i] = gSnap[i].req;
        gOut64[4][i] = gSnap[i].status;
        gOut64[5][i] = gSnap[i].pstate;
        gOut64[6][i] = gSnap[i].pstateDef;
    }

    publishArray(this, "cppc.cap1",   gOut64[0]);
    publishArray(this, "cppc.enable", gOut64[1]);
    publishArray(this, "cppc.cap2",   gOut64[2]);
    publishArray(this, "cppc.req",    gOut64[3]);
    publishArray(this, "cppc.status", gOut64[4]);
    publishArray(this, "pstate",      gOut64[5]);
    publishArray(this, "pstate.def",  gOut64[6]);

    uint32_t enabled = 0;
    for (uint32_t i = 0; i < gCpuCount; i++) {
        if ((gSnap[i].status & CPPC_STATUS_ENABLED) != 0) {
            enabled++;
        }
    }
    setProperty("cppc.enabled", (unsigned long long)enabled, 32);
    setProperty("cpu.count", (unsigned long long)gCpuCount, 32);

    uint32_t raw = 0;
    if (smnRead(SMN_THM_TCON_CUR_TMP, &raw)) {
        setProperty("temp.tctl", (unsigned long long)SMN_TCTL_MDEG(raw), 32);
        setProperty("temp.raw", (unsigned long long)raw, 32);
    } else {
        setProperty("temp.tctl", (unsigned long long)0xFFFFFFFFu, 32);
    }
}
