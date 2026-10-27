#pragma once
#include <IOKit/IOService.h>
struct IOTimerEventSource;
struct IOWorkLoop : OSObject { static IOWorkLoop *workLoop(); IOReturn addEventSource(IOTimerEventSource*); void removeEventSource(IOTimerEventSource*); };
