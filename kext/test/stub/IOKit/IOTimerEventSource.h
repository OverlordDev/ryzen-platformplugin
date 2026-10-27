#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
struct IOTimerEventSource : OSObject { typedef void (*Action)(OSObject*, IOTimerEventSource*);
  static IOTimerEventSource *timerEventSource(OSObject*, Action); void setTimeoutMS(unsigned); void cancelTimeout(); };
