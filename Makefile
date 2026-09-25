# whoport: make, make test, sudo make install
VERSION ?= 1.0.0
PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2
WARN = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral
SANITIZE ?= -fsanitize=address,undefined
DEFS = -DWHOPORT_VERSION=\"$(VERSION)\" -D_POSIX_C_SOURCE=200809L

SRC = src/main.c src/util.c src/ports_linux.c src/ports_macos.c
HDR = src/whoport.h

whoport: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) -o $@ $(SRC) $(LDFLAGS)

tests/test_util: tests/test_util.c src/util.c $(HDR)
	$(CC) -g -O1 $(WARN) $(DEFS) $(SANITIZE) -Isrc -o $@ tests/test_util.c src/util.c

test: whoport tests/test_util
	./tests/test_util
	./tests/integration.sh ./whoport

install: whoport
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 whoport $(DESTDIR)$(PREFIX)/bin/whoport

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/whoport

clean:
	rm -f whoport tests/test_util

.PHONY: test install uninstall clean
