/*
 * RyzenUserClient.hpp - the userspace door into the actuator.
 *
 * Root only, one client at a time. Closing the connection (including the daemon
 * dying) restores the firmware CPPC_REQ immediately, ahead of the 150 ms watchdog.
 * The client can call exactly the operations in shared/RyzenPlatformAPI.h; there
 * is no way through it to an arbitrary MSR or SMN address.
 */
#ifndef RYZEN_USERCLIENT_HPP
#define RYZEN_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>

class com_illia_RyzenPlatform;

class com_illia_RyzenUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(com_illia_RyzenUserClient)

public:
    virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                              OSDictionary *properties) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose(void) override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                    IOExternalMethodDispatch *dispatch = 0,
                                    OSObject *target = 0, void *reference = 0) override;

    com_illia_RyzenPlatform *owner(void) const { return fOwner; }

private:
    void releaseOwner(void);

    com_illia_RyzenPlatform *fOwner = nullptr;
    task_t                   fTask  = nullptr;
};

#endif /* RYZEN_USERCLIENT_HPP */
