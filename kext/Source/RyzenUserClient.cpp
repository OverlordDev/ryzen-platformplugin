/*
 * RyzenUserClient.cpp - see RyzenUserClient.hpp.
 *
 * Dispatch is the classic IOExternalMethodDispatch table: the base class checks
 * scalar/struct counts and sizes before our handler runs, so every handler can
 * trust the shapes. Handlers still validate the ABI version and let Actuator.cpp
 * validate the values.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>

#include "RyzenPlatformAPI.h"
#include "Actuator.hpp"
#include "RyzenPlatform.hpp"
#include "RyzenUserClient.hpp"

#define super IOUserClient
OSDefineMetaClassAndStructors(com_illia_RyzenUserClient, IOUserClient)

/* ------------------------------------------------------------ mapping */

static IOReturn
toIOReturn(int rc)
{
    switch (rc) {
    case RYZEN_OK:            return kIOReturnSuccess;
    case RYZEN_E_ABI:         return kIOReturnBadArgument;
    case RYZEN_E_RANGE:       return kIOReturnBadArgument;
    case RYZEN_E_UNSUPPORTED: return kIOReturnUnsupported;
    case RYZEN_E_THERMAL:     return kIOReturnNotPermitted;
    case RYZEN_E_DISABLED:    return kIOReturnNotPermitted;
    case RYZEN_E_LOCKOUT:     return kIOReturnNotPermitted;
    case RYZEN_E_WATCHDOG:    return kIOReturnTimeout;
    case RYZEN_E_READBACK:    return kIOReturnIOError;
    default:                  return kIOReturnInternalError;
    }
}

static bool
abiOk(IOExternalMethodArguments *a)
{
    return ryzen_abi_compatible((uint32_t)a->scalarInput[0]) != 0;
}

/* ------------------------------------------------------------ methods */

static IOReturn
mGetTopology(OSObject *, void *, IOExternalMethodArguments *a)
{
    if (!abiOk(a)) return kIOReturnBadArgument;
    a->structureOutputSize = sizeof(ryzen_topology_t);
    return toIOReturn(actTopology((ryzen_topology_t *)a->structureOutput));
}

static IOReturn
mReadState(OSObject *, void *, IOExternalMethodArguments *a)
{
    if (!abiOk(a)) return kIOReturnBadArgument;
    a->structureOutputSize = sizeof(ryzen_core_state_t);
    return toIOReturn(actReadState((uint32_t)a->scalarInput[1], (ryzen_core_state_t *)a->structureOutput));
}

static IOReturn
mApplyPerf(OSObject *target, void *, IOExternalMethodArguments *a)
{
    int rc = actApply((const ryzen_apply_t *)a->structureInput);
    /* Even a failed apply may have left an override active; make sure the
     * watchdog timer is running whenever one is. */
    com_illia_RyzenUserClient *uc = static_cast<com_illia_RyzenUserClient *>(target);
    if (uc && uc->owner()) {
        uc->owner()->kickWatchdog();
    }
    return toIOReturn(rc);
}

static IOReturn
mResetAuto(OSObject *, void *, IOExternalMethodArguments *a)
{
    if (!abiOk(a)) return kIOReturnBadArgument;
    return toIOReturn(actResetAuto("resetAuto from client", true));
}

static IOReturn
mHeartbeat(OSObject *, void *, IOExternalMethodArguments *a)
{
    return toIOReturn(actHeartbeat((const ryzen_heartbeat_t *)a->structureInput));
}

static IOReturn
mGetLastError(OSObject *, void *, IOExternalMethodArguments *a)
{
    if (!abiOk(a)) return kIOReturnBadArgument;
    a->structureOutputSize = sizeof(ryzen_error_t);
    actLastError((ryzen_error_t *)a->structureOutput);
    return kIOReturnSuccess;
}

static IOReturn
mGetStatus(OSObject *, void *, IOExternalMethodArguments *a)
{
    if (!abiOk(a)) return kIOReturnBadArgument;
    a->structureOutputSize = sizeof(ryzen_status_t);
    actStatus((ryzen_status_t *)a->structureOutput);
    return kIOReturnSuccess;
}

/* Index == ryzen_selector_t. Order is the ABI; do not reorder. */
static const IOExternalMethodDispatch sMethods[kRyzenSelectorCount] = {
    /* GetTopology  */ { (IOExternalMethodAction)&mGetTopology,  1, 0,                         0, sizeof(ryzen_topology_t)   },
    /* ReadState    */ { (IOExternalMethodAction)&mReadState,    2, 0,                         0, sizeof(ryzen_core_state_t) },
    /* ApplyPerf    */ { (IOExternalMethodAction)&mApplyPerf,    0, sizeof(ryzen_apply_t),     0, 0                          },
    /* ResetAuto    */ { (IOExternalMethodAction)&mResetAuto,    1, 0,                         0, 0                          },
    /* Heartbeat    */ { (IOExternalMethodAction)&mHeartbeat,    0, sizeof(ryzen_heartbeat_t), 0, 0                          },
    /* GetLastError */ { (IOExternalMethodAction)&mGetLastError, 1, 0,                         0, sizeof(ryzen_error_t)      },
    /* GetStatus    */ { (IOExternalMethodAction)&mGetStatus,    1, 0,                         0, sizeof(ryzen_status_t)     },
};

/* -------------------------------------------------------------- class */

bool
com_illia_RyzenUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
                                        OSDictionary *properties)
{
    if (!super::initWithTask(owningTask, securityID, type, properties)) {
        return false;
    }
    fTask = owningTask;
    return true;
}

bool
com_illia_RyzenUserClient::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }
    fOwner = OSDynamicCast(com_illia_RyzenPlatform, provider);
    return fOwner != nullptr;
}

void
com_illia_RyzenUserClient::releaseOwner(void)
{
    if (fOwner) {
        com_illia_RyzenPlatform *o = fOwner;
        fOwner = nullptr;
        o->clientClosed();
    }
}

IOReturn
com_illia_RyzenUserClient::clientClose(void)
{
    /* The daemon went away or closed the connection: give the CPUs back to the
     * firmware right now instead of waiting for the watchdog. */
    actResetAuto("client closed", false);
    releaseOwner();
    terminate();
    return kIOReturnSuccess;
}

void
com_illia_RyzenUserClient::stop(IOService *provider)
{
    actResetAuto("client stopped", false);
    releaseOwner();
    super::stop(provider);
}

IOReturn
com_illia_RyzenUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                          IOExternalMethodDispatch *dispatch, OSObject *target,
                                          void *reference)
{
    if (selector >= kRyzenSelectorCount) {
        return kIOReturnUnsupported;
    }
    if (!fOwner) {
        return kIOReturnNotAttached;
    }
    dispatch  = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target    = this;
    reference = nullptr;
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}
