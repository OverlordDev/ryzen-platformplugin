#pragma once
#include <IOKit/IOService.h>
struct IOExternalMethodArguments { const uint64_t *scalarInput; uint32_t scalarInputCount; const void *structureInput; uint32_t structureInputSize; void *structureOutput; uint32_t structureOutputSize; };
typedef IOReturn (*IOExternalMethodAction)(OSObject *target, void *reference, IOExternalMethodArguments *arguments);
struct IOExternalMethodDispatch { IOExternalMethodAction function; uint32_t checkScalarInputCount; uint32_t checkStructureInputSize; uint32_t checkScalarOutputCount; uint32_t checkStructureOutputSize; };
struct IOUserClient : IOService {
  virtual bool initWithTask(task_t, void*, UInt32, OSDictionary*);
  virtual IOReturn clientClose(void);
  virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments*, IOExternalMethodDispatch* = 0, OSObject* = 0, void* = 0);
  static IOReturn clientHasPrivilege(void*, const char*);
};
