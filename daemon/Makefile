# ryzend - userspace daemon. Plain make, no Xcode.
#   make            -> ./ryzend
CC     ?= xcrun clang
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LIBS   := -framework IOKit -framework CoreFoundation -framework ApplicationServices

all: ryzend

ryzend: ryzend.c ../shared/RyzenPlatformAPI.h
	$(CC) $(CFLAGS) -o $@ ryzend.c $(LIBS)

clean:
	rm -f ryzend

.PHONY: all clean
