/*
 * RyzenPlatform.hpp - v0.1 "eyes": read-only CPPC / P-state / temperature
 * sampler. It never writes an MSR.
 */
#ifndef RYZEN_PLATFORM_HPP
#define RYZEN_PLATFORM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

class com_illia_RyzenPlatform : public IOService
{
    OSDeclareDefaultStructors(com_illia_RyzenPlatform)

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;

private:
    void onTimer();
    void sample();

    IOWorkLoop         *fWorkLoop = nullptr;
    IOTimerEventSource *fTimer    = nullptr;
};

#endif /* RYZEN_PLATFORM_HPP */
