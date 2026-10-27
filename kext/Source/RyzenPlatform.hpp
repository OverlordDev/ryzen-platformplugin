/*
 * RyzenPlatform.hpp - v0.2: the 1 Hz sampler from v0.1 plus the CPPC actuator.
 * Writes are compiled in but OFF unless the machine was booted with
 * ryzenplatform_write=1; without it this kext is still read-only.
 */
#ifndef RYZEN_PLATFORM_HPP
#define RYZEN_PLATFORM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <stdint.h>

/* Sleep/wake through the IOKit power tree. UNTESTED and off by default: if
 * joinPMtree() on an IOResources provider misbehaves it can keep the kext from
 * loading. The daemon resets before sleep on its own (IORegisterForSystemPower),
 * which is the primary mechanism. Build with -DRYZEN_ENABLE_PM=1 to try this one. */
#ifndef RYZEN_ENABLE_PM
#define RYZEN_ENABLE_PM 0
#endif

class com_illia_RyzenPlatform : public IOService
{
    OSDeclareDefaultStructors(com_illia_RyzenPlatform)

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler) override;
#if RYZEN_ENABLE_PM
    virtual IOReturn setPowerState(unsigned long state, IOService *device) override;
#endif

    /* Called by the user client. */
    void clientClosed(void);
    void kickWatchdog(void);

private:
    void onTimer();
    void onWatchdog();
    void sample();
    void teardown();

    IOWorkLoop         *fWorkLoop   = nullptr;
    IOTimerEventSource *fTimer      = nullptr;   /* 1 Hz sampler            */
    IOTimerEventSource *fWatchTimer = nullptr;   /* 50 ms, only while an override is active */
    bool                fClientOpen = false;
    bool                fPmInit     = false;
};

/* ------------------------------------------------------------------------- */
/* Read access to what the sampler last saw. Used by Actuator.cpp.            */
/* ------------------------------------------------------------------------- */

struct RyzenCpuSample {
    uint64_t cap1, enable, cap2, req, status, pstate, pstateDef;   /* ~0ull = read failed */
};

/* False if cpu is out of range. The data is the last 1 Hz sample, up to a second old. */
bool     ryzenGetSample(uint32_t cpu, RyzenCpuSample *out);
/* Fresh SMN read of Tctl in millidegrees C (signed). False if the sensor did not answer
 * or the value is implausible; callers must treat that as "no data", not as "cool". */
bool     ryzenReadTctl(int32_t *mdeg);
uint32_t ryzenSmnRetries(void);

#endif /* RYZEN_PLATFORM_HPP */
