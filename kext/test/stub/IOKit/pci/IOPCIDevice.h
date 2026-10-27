#pragma once
#include <IOKit/IOService.h>
struct IOPCIDevice : IOService { UInt8 getBusNumber(); UInt8 getDeviceNumber(); UInt8 getFunctionNumber();
  void configWrite32(UInt8, UInt32); UInt32 configRead32(UInt8); };
