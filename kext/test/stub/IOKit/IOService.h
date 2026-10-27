#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int IOReturn; typedef uint8_t UInt8; typedef uint32_t UInt32; typedef void *task_t;
#define kIOReturnSuccess 0
#define kIOReturnBadArgument 1
#define kIOReturnUnsupported 2
#define kIOReturnNotPermitted 3
#define kIOReturnTimeout 4
#define kIOReturnIOError 5
#define kIOReturnInternalError 6
#define kIOReturnNotPrivileged 7
#define kIOReturnExclusiveAccess 8
#define kIOReturnNoMemory 9
#define kIOReturnNotAttached 10
#define IOPMAckImplied 0
#define kIOPMPowerOn 1
struct IOPMPowerState { unsigned long version, capabilityFlags, outputPowerCharacter, inputPowerRequirement, staticPower, unbudgetedPower, powerToAttain, timeToAttain, settleUpTime, timeToLower, settleDownTime, powerDomainBudget; };
struct OSObject { virtual ~OSObject(){} void retain(){} void release(){} };
struct OSBoolean : OSObject {}; extern OSBoolean *kOSBooleanTrue, *kOSBooleanFalse;
struct OSData : OSObject { static OSData *withBytes(const void*, unsigned); };
struct OSIterator : OSObject { OSObject *getNextObject(); };
struct OSDictionary;
struct IOService : OSObject {
  virtual bool start(IOService*); virtual void stop(IOService*); virtual void free();
  virtual bool attach(IOService*); virtual void detach(IOService*);
  virtual IOReturn newUserClient(task_t, void*, UInt32, OSDictionary*, struct IOUserClient**);
  virtual IOReturn setPowerState(unsigned long, IOService*);
  void PMinit(); void PMstop(); void joinPMtree(IOService*); IOReturn registerPowerDriver(IOService*, IOPMPowerState*, unsigned long);
  bool setProperty(const char*, OSObject*); bool setProperty(const char*, const char*); bool setProperty(const char*, unsigned long long, unsigned);
  void removeProperty(const char*); void registerService(); bool terminate(unsigned = 0);
  static OSIterator *getMatchingServices(OSDictionary*); static OSDictionary *serviceMatching(const char*);
  const char *getName();
};
#define OSDeclareDefaultStructors(c)
#define OSDefineMetaClassAndStructors(c,s)
#define OSMemberFunctionCast(t,o,f) ((t)0)
#define OSTypeAlloc(T) (new T)
template<class T> T *OSDynamicCast_(OSObject*o){return (T*)o;}
#define OSDynamicCast(T,o) OSDynamicCast_<T>(o)
