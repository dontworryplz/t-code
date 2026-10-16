/* Keys and mouse: moving around, the bottom-line prompt, clicking, dragging and the shortcuts. */
#include "fm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Reads input; returns the byte count, 0 when interrupted by a resize, -1 at end of input. */
int read_input(unsigned char *buf, size_t cap)
{
    ssize_t n = read(STDIN_FILENO, buf, cap);

    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        if (resized) {
            resized = 0;
            update_size();
        }
        return 0;
    }
    return n > 0 ? (int)n : -1;
}

/* Length of the escape sequence at buf[0] (ESC), or 1 for a lone ESC. */
int escape_len(const unsigned char *buf, int n)
{
    if (n < 2 || (buf[1] != '[' && buf[1] != 'O'))
        return 1;
    for (int i = 2; i < n; i++)
        if (buf[i] >= 0x40 && buf[i] <= 0x7E)
            return i + 1;
    return n;
}

/* Bottom-line input. Returns false when cancelled with Escape. */
bool prompt(const char *label, char *out, size_t cap, const char *initial)
{
    unsigned char buf[256];

    snprintf(out, cap, "%s", initial ? initial : "");
    for (;;) {
        draw(label, out);
        int n = read_input(buf, sizeof buf);
        if (n < 0)
            exit(0);
        for (int i = 0; i < n; i++) {
            unsigned char c = buf[i];
            size_t len = strlen(out);
            if (c == '\r' || c == '\n') {
                return true;
            } else if (c == 0x1B) {
                int skip = escape_len(buf + i, n - i);
                if (skip == 1)
                    return false;
                i += skip - 1;
            } else if (c == 0x7F || c == 0x08) {
                while (len > 0 && (out[len - 1] & 0xC0) == 0x80)
                    out[--len] = '\0';
                if (len > 0)
                    out[--len] = '\0';
            } else if (c == 0x03 || c == 0x07) { /* Ctrl+C, Ctrl+G */
                return false;
            } else if (c >= 0x20 && len + 1 < cap) {
                out[len] = c;
                out[len + 1] = '\0';
            }
        }
    }
}

bool confirm(const char *question)
{
    char answer[8];

    return prompt(question, answer, sizeof answer, NULL) && (answer[0] == 'y' || answer[0] == 'Y');
}

/* Mouse -------------------------------------------------------------------------------------- */

/* Where a drop on screen line y goes: the folder under the pointer, a file's folder, or the root. */
static void drop_dir(int y, char *out, size_t cap)
{
    int r = row_at(y);

    snprintf(out, cap, "%s", r >= 0 ? fm.rows[r].path : fm.root);
    if (r >= 0 && !fm.rows[r].dir)
        *strrchr(out, '/') = '\0';
}

/* Left button: a press selects; a release on the same row opens it, unless the pointer moved to
 * another row in between, which drags the row there (see draw_row for the highlight). */
static void handle_mouse(int button, int x, int y, bool press)
{
    char dir[PATH_MAX];

    if (button == 64 || button == 65) { /* wheel */
        if (press) {
            fm.top += button == 64 ? -3 : 3;
            fm.follow = false;
        }
        return;
    }
    if (button == 2 && press) {
        context_menu(row_at(y), x, y);
        return;
    }
    if (button == 0 && press) {
        fm.press_row = row_at(y);
        fm.dragging = false;
        if (fm.press_row >= 0) {
            fm.sel = fm.press_row;
            fm.follow = true;
        }
        return;
    }
    if (button == 32 && press) { /* moving with the left button down */
        if (fm.press_row < 0)
            return;
        int r = row_at(y);
        if (!fm.dragging && r != fm.press_row)
            fm.dragging = true;
        if (fm.dragging) {
            /* Highlight the folder it would go into; -1 is the root (its name on the first line). */
            drop_dir(y, dir, sizeof dir);
            fm.drop_row = -1;
            for (int i = 0; i < fm.nrows; i++)
                if (strcmp(fm.rows[i].path, dir) == 0)
                    fm.drop_row = i;
            message("Move %s into %s/", fm.rows[fm.press_row].name, *relative(dir) ? relative(dir) : ".");
        }
        return;
    }
    if (button == 0 && !press) {
        int pressed = fm.press_row;
        fm.press_row = -1;
        if (fm.dragging) {
            char src[PATH_MAX];
            fm.dragging = false;
            fm.message[0] = '\0';
            snprintf(src, sizeof src, "%s", fm.rows[pressed].path);
            drop_dir(y, dir, sizeof dir);
            move_into(src, dir);
        } else if (pressed >= 0 && pressed == row_at(y)) {
            activate();
        }
    }
}

/* Keys --------------------------------------------------------------------------------------- */

static void handle_csi(const unsigned char *seq, int len)
{
    char params[64] = "";
    unsigned char final = seq[len - 1];
    int plen = len - 3;

    if (plen > 0 && plen < (int)sizeof params)
        memcpy(params, seq + 2, plen);

    if (params[0] == '<' && (final == 'M' || final == 'm')) {
        int b, x, y;
        if (sscanf(params + 1, "%d;%d;%d", &b, &x, &y) == 3)
            handle_mouse(b, x, y, final == 'M');
        return;
    }
    int page = body_height(fm.message[0]) - 1;
    switch (final) {
    case 'A': move(-1); break;
    case 'B': move(1); break;
    case 'C': activate(); break;
    case 'D': collapse(); break;
    case 'H': move(-fm.nrows); break;
    case 'F': move(fm.nrows); break;
    case 'I': fm.focused = true; break;
    case 'O': fm.focused = false; break;
    case '~':
        switch (atoi(params)) {
        case 1: case 7: move(-fm.nrows); break;
        case 4: case 8: move(fm.nrows); break;
        case 5: move(-page); break;
        case 6: move(page); break;
        case 3: delete_selected(); break;
        }
        break;
    }
}

void handle_input(const unsigned char *buf, int n)
{
    char dir[PATH_MAX];

    for (int i = 0; i < n; i++) {
        unsigned char c = buf[i];

        if (c != 0x1B || escape_len(buf + i, n - i) == 1)
            fm.message[0] = '\0';
        switch (c) {
        case 0x1B: {
            int len = escape_len(buf + i, n - i);
            if (len >= 3 && buf[i + 1] == '[') {
                handle_csi(buf + i, len);
            } else if (len == 3 && buf[i + 1] == 'O') { /* SS3 arrows, Home, End */
                unsigned char seq[3] = { 0x1B, '[', buf[i + 2] };
                handle_csi(seq, 3);
            }
            i += len - 1;
            break;
        }
        case 'j': move(1); break;
        case 'k': move(-1); break;
        case 'g': move(-fm.nrows); break;
        case 'G': move(fm.nrows); break;
        case '\r': case 'l': activate(); break;
        case 'h': case 0x7F: case 0x08: collapse(); break;
        case 'a': create(); break;
        case 'r': rename_selected(); break;
        case 'd': delete_selected(); break;
        case 0x18: clip_selected(true); break;  /* Ctrl+X */
        case 0x03: clip_selected(false); break; /* Ctrl+C */
        case 0x16:                              /* Ctrl+V */
            target_dir(dir, sizeof dir);
            paste_into(dir);
            break;
        case '.':
            fm.show_hidden = !fm.show_hidden;
            rebuild(selected_path());
            break;
        case 'R': rebuild(selected_path()); break;
        }
    }
}
