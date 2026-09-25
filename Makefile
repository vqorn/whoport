# whoport: make, make test, sudo make install
# Cross-compile for Windows from Linux: make CC=x86_64-w64-mingw32-gcc OS=Windows_NT
VERSION ?= 1.2.2
PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2
DEFS = -DWHOPORT_VERSION=\"$(VERSION)\"

ifeq ($(OS),Windows_NT)
  EXE = .exe
  # Windows headers are not -Wpedantic clean, and MinGW has no sanitizers.
  WARN = -std=gnu11 -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral
  LDLIBS = -liphlpapi -lws2_32 -lpsapi -lntdll
  SANITIZE ?=
else
  EXE =
  WARN = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral
  DEFS += -D_POSIX_C_SOURCE=200809L
  SANITIZE ?= -fsanitize=address,undefined
endif

SRC = src/main.c src/util.c src/docker.c src/platform_posix.c src/ports_linux.c src/ports_macos.c src/ports_windows.c
HDR = src/whoport.h
BIN = whoport$(EXE)
TEST_BIN = tests/test_util$(EXE)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): tests/test_util.c src/util.c src/docker.c src/platform_posix.c src/ports_windows.c $(HDR)
	$(CC) -g -O1 $(WARN) $(DEFS) $(SANITIZE) -Isrc -o $@ tests/test_util.c src/util.c src/docker.c src/platform_posix.c src/ports_windows.c $(LDLIBS)

test: $(BIN) $(TEST_BIN)
	./$(TEST_BIN)
	./tests/integration.sh ./$(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f whoport whoport.exe tests/test_util tests/test_util.exe

.PHONY: test install uninstall clean
