/* Drawing the tree, the bottom line (prompt or message) and the output buffer. */
#include "fm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

/* Output buffer ------------------------------------------------------------------------------ */

void buf_add(Buf *b, const char *s, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 256;
        b->data = realloc(b->data, b->cap);
        if (!b->data)
            abort();
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

void buf_puts(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_add(b, tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static int next_char(const char *s, size_t *bytes)
{
    wchar_t wc;
    mbstate_t st = { 0 };
    size_t n = mbrtowc(&wc, s, 4, &st);

    if (n == 0 || n == (size_t)-1 || n == (size_t)-2) {
        *bytes = 1;
        return 1;
    }
    *bytes = n;
    int w = wcwidth(wc);
    return w >= 0 ? w : 1;
}

/* Appends at most `cols` columns of UTF-8 text s; returns the columns used. */
int buf_cols(Buf *b, const char *s, int cols)
{
    int used = 0;

    for (const char *p = s; *p;) {
        size_t n;
        int w = next_char(p, &n);
        if (used + w > cols)
            break;
        buf_add(b, p, n);
        used += w;
        p += n;
    }
    return used;
}

/* Writes the buffer to the terminal and empties it. */
void buf_write(Buf *b)
{
    if (write(STDOUT_FILENO, b->data, b->len) < 0) {
        /* Nothing useful to do: the terminal is gone. */
    }
    free(b->data);
    *b = (Buf){ 0 };
}

int utf8_cols(const char *s)
{
    int cols = 0;

    for (const char *p = s; *p;) {
        size_t n;
        cols += next_char(p, &n);
        p += n;
    }
    return cols;
}

/* The bottom line: a note until the next key. */
static void set_message(bool error, const char *fmt, va_list ap)
{
    vsnprintf(fm.message, sizeof fm.message, fmt, ap);
    fm.message_is_error = error;
}

void message(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    set_message(false, fmt, ap);
    va_end(ap);
}

void error_message(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    set_message(true, fmt, ap);
    va_end(ap);
}

/* Layout ------------------------------------------------------------------------------------- */

void update_size(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        fm.width = ws.ws_col;
        fm.height = ws.ws_row;
    } else {
        fm.width = 30;
        fm.height = 24;
    }
}

int body_height(bool bottom_line)
{
    int h = fm.height - 1 - (bottom_line ? 1 : 0);
    return h > 0 ? h : 1;
}

static void draw_row(Buf *b, const Row *row, int index)
{
    int cols = fm.width;
    bool selected = index == fm.sel && !fm.dragging;
    bool drop = fm.dragging && index == fm.drop_row;
    bool cut = fm.clip_cut && strcmp(fm.clip, row->path) == 0;

    if (selected)
        buf_puts(b, fm.focused ? BG_SELECTED : BG_INACTIVE);
    else if (drop)
        buf_puts(b, BG_DROP);
    for (int i = 0; i < row->depth && cols >= 2; i++, cols -= 2)
        buf_puts(b, "  ");
    if (cols >= 2) {
        if (row->dir) {
            buf_puts(b, COLOR_CHEVRON);
            buf_puts(b, is_expanded(row->path) >= 0 ? "▾ " : "▸ ");
        } else {
            buf_puts(b, "  ");
        }
        cols -= 2;
    }
    const char *badge = row->dir ? NULL : git_badge(row->git);
    int name_cols = badge && cols > 2 ? cols - 2 : cols;
    buf_puts(b, cut ? COLOR_DIM : git_color(row->git));
    cols -= buf_cols(b, row->name, name_cols);
    if (badge && cols >= 2) {
        while (cols > 2) {
            buf_add(b, " ", 1);
            cols--;
        }
        buf_puts(b, badge);
        buf_add(b, " ", 1);
        cols -= 2;
    }
    if (selected || drop)
        while (cols-- > 0)
            buf_add(b, " ", 1);
    buf_puts(b, RESET "\x1b[K");
}

/* The tree, and the bottom line: an input field when prompt is given, otherwise fm.message, if any. */
void draw(const char *prompt, const char *input)
{
    bool bottom = prompt || fm.message[0];
    int body = body_height(bottom);
    Buf b = { 0 };

    if (fm.follow) {
        if (fm.sel < fm.top)
            fm.top = fm.sel;
        if (fm.sel >= fm.top + body)
            fm.top = fm.sel - body + 1;
    }
    if (fm.top > fm.nrows - body)
        fm.top = fm.nrows - body;
    if (fm.top < 0)
        fm.top = 0;

    buf_puts(&b, "\x1b[?25l\x1b[1;1H" RESET "\x1b[1m" COLOR_TEXT);
    if (fm.dragging && fm.drop_row < 0)
        buf_puts(&b, BG_DROP); /* dropping on the root */
    const char *name = strrchr(fm.root, '/');
    int used = buf_cols(&b, name && name[1] ? name + 1 : fm.root, fm.width);
    if (fm.dragging && fm.drop_row < 0)
        while (used++ < fm.width)
            buf_add(&b, " ", 1);
    buf_puts(&b, RESET "\x1b[K");

    for (int i = 0; i < body; i++) {
        int r = fm.top + i;
        buf_printf(&b, "\x1b[%d;1H", i + 2);
        if (r < fm.nrows)
            draw_row(&b, &fm.rows[r], r);
        else
            buf_puts(&b, RESET "\x1b[K");
    }

    if (bottom) {
        buf_printf(&b, "\x1b[%d;1H" RESET, fm.height);
        if (prompt) {
            int cols = fm.width - buf_cols(&b, prompt, fm.width);
            /* Show the end of the input when it does not fit. */
            const char *tail = input;
            int len = utf8_cols(input);
            while (len >= cols && *tail) {
                tail++;
                while ((*tail & 0xC0) == 0x80)
                    tail++;
                len--;
            }
            buf_cols(&b, tail, cols);
            buf_puts(&b, "\x1b[K\x1b[?25h");
        } else {
            buf_puts(&b, fm.message_is_error ? COLOR_ERROR : COLOR_TEXT);
            buf_cols(&b, fm.message, fm.width);
            buf_puts(&b, RESET "\x1b[K");
        }
    }
    buf_write(&b);
}
