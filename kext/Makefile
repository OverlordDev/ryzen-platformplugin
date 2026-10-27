# Command-line build of RyzenPlatform.kext, no Xcode project needed.
# Same recipe as RyzenHV.kext, which already builds and loads on this machine.
SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
KFW     := $(SDK)/System/Library/Frameworks/Kernel.framework
OUT     ?= ../build/run
KEXT    := $(OUT)/RyzenPlatform.kext
OBJDIR  := $(OUT)/kext-obj

CXXSRC  := $(wildcard Source/*.cpp)
CSRC    := $(wildcard Source/*.c)
OBJS    := $(patsubst Source/%.cpp,$(OBJDIR)/%.o,$(CXXSRC)) \
           $(patsubst Source/%.c,$(OBJDIR)/%.o,$(CSRC))

DEBUG   ?= 0
ifeq ($(DEBUG),1)
OPT     := -O0 -g
else
OPT     := -O2
endif

COMMON  := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=10.15 \
           -mkernel -nostdinc -nostdlibinc \
           -I$(KFW)/Headers -I$(KFW)/PrivateHeaders \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -Wall -Wextra $(OPT)

CXXFLAGS := $(COMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fno-builtin
CFLAGS   := $(COMMON) -fno-builtin

.PHONY: all clean

all: $(KEXT)

$(OBJDIR)/%.o: Source/%.cpp
	@mkdir -p $(OBJDIR)
	xcrun clang++ $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: Source/%.c
	@mkdir -p $(OBJDIR)
	xcrun clang $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp Info.plist $(KEXT)/Contents/Info.plist
	xcrun clang++ -arch x86_64 -isysroot $(SDK) -mkernel -nostdlib -fapple-kext \
		-Xlinker -kext -L$(SDK)/usr/lib \
		-o $(KEXT)/Contents/MacOS/RyzenPlatform $(OBJS) \
		-lkmodc++ -lkmod -lcc_kext
	@echo "Built $(KEXT)"

clean:
	rm -rf $(KEXT) $(OBJDIR)
