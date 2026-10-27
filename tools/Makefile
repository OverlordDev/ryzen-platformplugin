CC ?= xcrun clang
CFLAGS ?= -O2 -Wall -Wextra

all: ryzenstat

ryzenstat: ryzenstat.c
	$(CC) $(CFLAGS) -o $@ $< -framework IOKit -framework CoreFoundation

clean:
	rm -f ryzenstat
