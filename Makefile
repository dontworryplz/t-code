VERSION := 0.2
PKGS    := gtk4 gtksourceview-5 json-glib-1.0 vte-2.91-gtk4 gio-unix-2.0 md4c
# t-code: src/main.c and the feature folders (src/window, src/editor, ...); src/fm is the file
# manager, a program of its own.
APP_SRC := $(filter-out src/fm/%,$(wildcard src/*.c src/*/*.c))
APP_HDR := $(filter-out src/fm/%,$(wildcard src/*.h src/*/*.h))
FM_SRC  := $(wildcard src/fm/*.c)
TEST_SRC := $(wildcard tests/*.c)
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c23
# Where 'make install' puts things. t-code finds its helpers next to itself, so LIBEXECDIR must be
# $(PREFIX)/libexec or $(PREFIX)/lib, and DATADIR $(PREFIX)/share.
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
DATADIR    ?= $(PREFIX)/share
APP_CFLAGS := $(shell pkg-config --cflags $(PKGS)) -Isrc -Ibuild

all: build/t-code build/t-code-fm build/compile_commands.json

build/t-code: $(APP_SRC) $(APP_HDR) build/license.h Makefile | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o $@ $(APP_SRC) $(LDFLAGS) \
	  $(shell pkg-config --libs $(PKGS))

# LICENSE as a C string for the About window (src/dialogs/about.c).
build/license.h: LICENSE | build
	sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&\\n"/' $< > $@

build/t-code-fm: $(FM_SRC) src/fm/fm.h | build
	$(CC) $(CFLAGS) -o $@ $(FM_SRC) $(LDFLAGS)

# How each file is compiled, so clangd (in t-code, VS Code, ...) finds the GTK, VTE and md4c headers.
build/compile_commands.json: Makefile $(APP_SRC) $(FM_SRC) $(TEST_SRC) | build
	@( printf '['; sep=; \
	   for f in $(APP_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$$f"; sep=,; done; \
	   for f in $(TEST_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$$f"; sep=,; done; \
	   for f in $(FM_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS)" "$$f"; sep=,; done; \
	   printf '\n]\n' ) > $@

# Tests: searchmatch.c, editor/detect.c and git/linediff.c alone (GLib); the Search panel, editing, Source Control and
# About in a window (they need a display for some or all of their tests).
build/test-searchmatch: tests/test_searchmatch.c src/search/searchmatch.c src/search/searchmatch.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags gio-2.0) -o $@ tests/test_searchmatch.c src/search/searchmatch.c \
	  $(shell pkg-config --libs gio-2.0)

build/test-search-panel: tests/test_search_panel.c src/search/search.c src/search/searchmatch.c $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_search_panel.c src/search/search.c src/search/searchmatch.c \
	  $(shell pkg-config --libs $(PKGS))

build/test-linediff: tests/test_linediff.c src/git/linediff.c src/git/linediff.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags glib-2.0) -o $@ tests/test_linediff.c src/git/linediff.c \
	  $(shell pkg-config --libs glib-2.0)

build/test-detect: tests/test_detect.c src/editor/detect.c src/editor/detect.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags glib-2.0) -o $@ tests/test_detect.c src/editor/detect.c \
	  $(shell pkg-config --libs glib-2.0)

# The editor page's files (with the language server client) are included by the tests through
# tests/editor_page.h, to reach their static functions. test-lsp runs itself with --fake-server as
# the language server.
PAGE_SRC := $(wildcard src/editor/editing.c src/editor/findbar.c src/editor/editorview.c src/editor/detect.c \
              src/lsp/*.c) tests/editor_page.h
build/test-editing: tests/test_editing.c $(PAGE_SRC) $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_editing.c $(shell pkg-config --libs $(PKGS))

build/test-lsp: tests/test_lsp.c $(PAGE_SRC) $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_lsp.c $(shell pkg-config --libs $(PKGS))

build/test-scm: tests/test_scm.c src/git/scm.c src/git/gitstatus.c $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_scm.c src/git/scm.c $(shell pkg-config --libs $(PKGS))

build/test-about: tests/test_about.c src/dialogs/about.c $(APP_HDR) build/license.h | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o $@ tests/test_about.c \
	  $(shell pkg-config --libs $(PKGS))

test: build/test-searchmatch build/test-detect build/test-linediff build/test-lsp build/test-search-panel build/test-editing build/test-scm build/test-about
	build/test-searchmatch
	build/test-detect
	build/test-linediff
	build/test-lsp
	build/test-search-panel
	build/test-editing
	build/test-scm
	build/test-about

# The tests that need no display again, under AddressSanitizer and UBSan: out-of-bounds access, use
# after free, undefined behavior, and (LeakSanitizer, at exit) every block that was not freed.
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
              -Wall -Wextra -std=c23 -Isrc
ASAN_ENV   := G_DEBUG=gc-friendly ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
              LSAN_OPTIONS=suppressions=$(CURDIR)/tests/lsan.supp:print_suppressions=0

test-asan: | build
	mkdir -p build/asan
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags gio-2.0) -o build/asan/test-searchmatch \
	  tests/test_searchmatch.c src/search/searchmatch.c $(shell pkg-config --libs gio-2.0)
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags glib-2.0) -o build/asan/test-detect \
	  tests/test_detect.c src/editor/detect.c $(shell pkg-config --libs glib-2.0)
	$(ASAN_ENV) build/asan/test-searchmatch
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags glib-2.0) -o build/asan/test-linediff \
	  tests/test_linediff.c src/git/linediff.c $(shell pkg-config --libs glib-2.0)
	$(ASAN_ENV) build/asan/test-detect
	$(ASAN_ENV) build/asan/test-linediff

# System-wide install, used by the packages in packaging/ (install.sh installs into ~/.local).
install: build/t-code build/t-code-fm
	install -Dm755 build/t-code $(DESTDIR)$(BINDIR)/t-code
	install -Dm755 build/t-code-fm $(DESTDIR)$(LIBEXECDIR)/t-code/t-code-fm
	install -Dm644 styles/dark-modern.xml $(DESTDIR)$(DATADIR)/t-code/styles/dark-modern.xml
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
