PKGS    := gtk4 vte-2.91-gtk4 gio-unix-2.0 webkitgtk-6.0 md4c-html
APP_SRC := $(wildcard src/*.c)
FM_SRC  := $(wildcard src/fm/*.c)
LSP_SRC := $(wildcard src/lsp/*.c)
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c23
APP_CFLAGS := $(shell pkg-config --cflags $(PKGS))

all: build/t-code build/t-code-fm build/t-code-lsp build/compile_commands.json

build/t-code: $(APP_SRC) src/app.h | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ $(APP_SRC) $(shell pkg-config --libs $(PKGS))

build/t-code-fm: $(FM_SRC) src/fm/fm.h | build
	$(CC) $(CFLAGS) -o $@ $(FM_SRC)

build/t-code-lsp: $(LSP_SRC) | build
	$(CC) $(CFLAGS) -o $@ $(LSP_SRC)

# How each file is compiled, so clangd (in micro, VS Code, ...) finds the GTK, VTE and WebKit headers.
build/compile_commands.json: Makefile $(APP_SRC) $(FM_SRC) $(LSP_SRC) | build
	@( printf '['; sep=; \
	   for f in $(APP_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$$f"; sep=,; done; \
	   for f in $(FM_SRC) $(LSP_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS)" "$$f"; sep=,; done; \
	   printf '\n]\n' ) > $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
