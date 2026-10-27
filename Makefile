# Top-level convenience targets. Everything builds with plain make + Command Line Tools.
#   make            build kext, ryzenstat, ryzend
#   make test       host-side tests of the actuator's safety logic (no kernel needed)
all: kext tools daemon

kext:
	$(MAKE) -C kext
tools:
	$(MAKE) -C tools
daemon:
	$(MAKE) -C daemon
test:
	$(MAKE) -C kext/test
clean:
	$(MAKE) -C kext clean
	$(MAKE) -C tools clean
	$(MAKE) -C daemon clean
	$(MAKE) -C kext/test clean

.PHONY: all kext tools daemon test clean
