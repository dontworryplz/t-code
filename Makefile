VERSION := 0.1
PKGS    := gtk4 vte-2.91-gtk4 gio-unix-2.0 webkitgtk-6.0 md4c-html
APP_SRC := $(wildcard src/*.c)
FM_SRC  := $(wildcard src/fm/*.c)
LSP_SRC := $(wildcard src/lsp/*.c)
TEST_SRC := $(wildcard tests/*.c)
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c23
# Where 'make install' puts things. t-code finds its helpers next to itself, so LIBEXECDIR must be
# $(PREFIX)/libexec or $(PREFIX)/lib, and DATADIR $(PREFIX)/share.
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
DATADIR    ?= $(PREFIX)/share
APP_CFLAGS := $(shell pkg-config --cflags $(PKGS))

all: build/t-code build/t-code-fm build/t-code-lsp build/compile_commands.json

build/t-code: $(APP_SRC) src/app.h | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o $@ $(APP_SRC) $(LDFLAGS) \
	  $(shell pkg-config --libs $(PKGS))

build/t-code-fm: $(FM_SRC) src/fm/fm.h | build
	$(CC) $(CFLAGS) -o $@ $(FM_SRC) $(LDFLAGS)

build/t-code-lsp: $(LSP_SRC) | build
	$(CC) $(CFLAGS) -o $@ $(LSP_SRC) $(LDFLAGS)

# How each file is compiled, so clangd (in micro, VS Code, ...) finds the GTK, VTE and WebKit headers.
build/compile_commands.json: Makefile $(APP_SRC) $(FM_SRC) $(LSP_SRC) $(TEST_SRC) | build
	@( printf '['; sep=; \
	   for f in $(APP_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$$f"; sep=,; done; \
	   for f in $(TEST_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -Isrc -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$$f"; sep=,; done; \
	   for f in $(FM_SRC) $(LSP_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS)" "$$f"; sep=,; done; \
	   printf '\n]\n' ) > $@

# Tests: searchmatch.c alone (GLib); the Search panel, the minimap and Source Control in a window
# (they need a display for some or all of their tests).
build/test-searchmatch: tests/test_searchmatch.c src/searchmatch.c src/searchmatch.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags gio-2.0) -o $@ tests/test_searchmatch.c src/searchmatch.c \
	  $(shell pkg-config --libs gio-2.0)

build/test-search-panel: tests/test_search_panel.c src/search.c src/searchmatch.c src/searchmatch.h src/app.h | build
	$(CC) $(CFLAGS) -Isrc $(APP_CFLAGS) -o $@ tests/test_search_panel.c src/search.c src/searchmatch.c \
	  $(shell pkg-config --libs $(PKGS))

build/test-minimap: tests/test_minimap.c src/editorview.c src/app.h | build
	$(CC) $(CFLAGS) -Isrc $(APP_CFLAGS) -o $@ tests/test_minimap.c $(shell pkg-config --libs $(PKGS))

build/test-scm: tests/test_scm.c src/scm.c src/status.c src/app.h | build
	$(CC) $(CFLAGS) -Isrc $(APP_CFLAGS) -o $@ tests/test_scm.c src/scm.c $(shell pkg-config --libs $(PKGS))

# The shared language servers (src/lsp.c), with the test program itself as the server.
build/test-lsp: tests/test_lsp.c src/lsp.c src/app.h | build
	$(CC) $(CFLAGS) -Isrc $(APP_CFLAGS) -o $@ tests/test_lsp.c $(LDFLAGS) $(shell pkg-config --libs gio-unix-2.0)

test: build/test-searchmatch build/test-lsp build/test-search-panel build/test-minimap build/test-scm
	build/test-searchmatch
	build/test-lsp
	build/test-search-panel
	build/test-minimap
	build/test-scm

# The tests that need no display again, under AddressSanitizer and UBSan: out-of-bounds access, use
# after free, undefined behavior, and (LeakSanitizer, at exit) every block that was not freed.
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
              -Wall -Wextra -std=c23 -Isrc
ASAN_ENV   := G_DEBUG=gc-friendly ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
              LSAN_OPTIONS=suppressions=$(CURDIR)/tests/lsan.supp:print_suppressions=0

test-asan: | build
	mkdir -p build/asan
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags gio-2.0) -o build/asan/test-searchmatch \
	  tests/test_searchmatch.c src/searchmatch.c $(shell pkg-config --libs gio-2.0)
	$(CC) $(ASAN_FLAGS) $(APP_CFLAGS) -o build/asan/test-lsp tests/test_lsp.c \
	  $(shell pkg-config --libs gio-unix-2.0)
	$(ASAN_ENV) build/asan/test-searchmatch
	$(ASAN_ENV) build/asan/test-lsp

# System-wide install, used by the packages in packaging/ (install.sh installs into ~/.local).
MICRO_FILES := settings.json bindings.json colorschemes syntax plug bin

install: build/t-code build/t-code-fm build/t-code-lsp
	install -Dm755 build/t-code $(DESTDIR)$(BINDIR)/t-code
	install -Dm755 build/t-code-fm $(DESTDIR)$(LIBEXECDIR)/t-code/t-code-fm
	install -Dm755 build/t-code-lsp $(DESTDIR)$(LIBEXECDIR)/t-code/t-code-lsp
	install -d $(DESTDIR)$(DATADIR)/t-code/micro
	cd micro && cp -R $(MICRO_FILES) $(DESTDIR)$(DATADIR)/t-code/micro/
	chmod -R u=rwX,go=rX $(DESTDIR)$(DATADIR)/t-code
	install -Dm644 desktop/io.github.tcode.svg \
	  $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/io.github.tcode.svg
	install -d $(DESTDIR)$(DATADIR)/applications
	sed 's|@BIN@|$(BINDIR)/t-code|' desktop/io.github.tcode.desktop.in \
	  > $(DESTDIR)$(DATADIR)/applications/io.github.tcode.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/applications/io.github.tcode.desktop

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean test test-asan install
