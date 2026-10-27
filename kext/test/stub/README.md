Minimal fake IOKit/XNU headers, just enough for `Actuator.cpp` to compile on an ordinary host.
They are NOT a model of the real kernel API: never use them to decide whether a real build
will work. They exist only so `make -C kext/test` can exercise the actuator's safety logic.
