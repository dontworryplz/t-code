PKGS    := gtk4 vte-2.91-gtk4 gio-unix-2.0 webkitgtk-6.0 md4c-html
APP_SRC := src/t-code.c src/run.c src/session.c src/quickopen.c src/preview.c
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c17

all: build/t-code build/t-code-fm compile_flags.txt

build/t-code: $(APP_SRC) src/app.h | build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKGS)) -o $@ $(APP_SRC) $(shell pkg-config --libs $(PKGS))

build/t-code-fm: src/fm.c | build
	$(CC) $(CFLAGS) -o $@ $<

# Lets clangd find the GTK, VTE and WebKit headers while editing this code.
compile_flags.txt: Makefile
	printf '%s\n' -std=c17 -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) > $@

build:
	mkdir -p build

clean:
	rm -rf build compile_flags.txt

.PHONY: all clean
