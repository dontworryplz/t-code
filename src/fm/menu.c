/* The right-click menu, drawn as a box at the click; mouse hover and arrow keys pick an item. */
#include "fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    ACT_NONE = -1, ACT_SEPARATOR, ACT_OPEN, ACT_OPEN_SIDE, ACT_PREVIEW, ACT_PROJECT, ACT_COMPARE,
    ACT_HISTORY, ACT_NEW_FILE, ACT_NEW_FOLDER, ACT_TERMINAL, ACT_CUT, ACT_COPY, ACT_PASTE,
    ACT_COPY_PATH, ACT_COPY_RELATIVE, ACT_RENAME, ACT_DELETE
};

typedef struct {
    const char *label;
    int action;
} MenuItem;

#define MAX_ITEMS 32

/* The items for a row (NULL: the empty space below the rows). */
static int build_menu(const Row *row, MenuItem *items)
{
    int n = 0;
#define ADD(label, action) (items[n++] = (MenuItem){ label, action })
    if (!row) {
        ADD("New File…", ACT_NEW_FILE);
        ADD("New Folder…", ACT_NEW_FOLDER);
        ADD("Open in Terminal", ACT_TERMINAL);
        ADD(NULL, ACT_SEPARATOR);
        ADD("Paste", ACT_PASTE);
        return n;
    }
    ADD("Open", ACT_OPEN);
    if (row->dir) {
        ADD("Open Folder as Project", ACT_PROJECT);
    } else {
        ADD("Open to the Side", ACT_OPEN_SIDE);
        if (is_markdown(row))
            ADD("Open Preview", ACT_PREVIEW);
        if (fm.git_dir[0]) {
            ADD(NULL, ACT_SEPARATOR);
            ADD("Compare with HEAD", ACT_COMPARE);
            ADD("File History…", ACT_HISTORY);
        }
    }
    ADD(NULL, ACT_SEPARATOR);
    ADD("New File…", ACT_NEW_FILE);
    ADD("New Folder…", ACT_NEW_FOLDER);
    ADD("Open in Terminal", ACT_TERMINAL);
    ADD(NULL, ACT_SEPARATOR);
    ADD("Cut", ACT_CUT);
    ADD("Copy", ACT_COPY);
    ADD("Paste", ACT_PASTE);
    ADD(NULL, ACT_SEPARATOR);
    ADD("Copy Path", ACT_COPY_PATH);
    ADD("Copy Relative Path", ACT_COPY_RELATIVE);
    ADD(NULL, ACT_SEPARATOR);
    ADD("Rename…", ACT_RENAME);
    ADD("Delete", ACT_DELETE);
#undef ADD
    return n;
}

/* Drawing ------------------------------------------------------------------------------------ */

typedef struct {
    const MenuItem *items;
    int n, sel;
    int left, top, inner; /* 1-based screen position of the box; inner = width inside the border */
} Menu;

/* Puts the box at the click, moved left or up when it would not fit. */
static void place_menu(Menu *m, int x, int y)
{
    int widest = 0;

    for (int i = 0; i < m->n; i++)
        if (m->items[i].label && utf8_cols(m->items[i].label) > widest)
            widest = utf8_cols(m->items[i].label);
    m->inner = widest + 4;
    if (m->inner > fm.width - 2)
        m->inner = fm.width - 2 > 1 ? fm.width - 2 : 1;
    m->left = x + m->inner + 1 <= fm.width ? x : fm.width - m->inner - 1;
    m->top = y + m->n + 1 <= fm.height ? y : y - m->n - 1;
    if (m->left < 1)
        m->left = 1;
    if (m->top < 1)
        m->top = 1;
}

static void draw_menu(const Menu *m)
{
    Buf b = { 0 };

    for (int line = 0; line < m->n + 2 && m->top + line <= fm.height; line++) {
        const MenuItem *item = line > 0 && line <= m->n ? &m->items[line - 1] : NULL;
        const char *l = line == 0 ? "┌" : line == m->n + 1 ? "└" : item->label ? "│" : "├";
        const char *r = line == 0 ? "┐" : line == m->n + 1 ? "┘" : item->label ? "│" : "┤";

        buf_printf(&b, "\x1b[%d;%dH" BG_MENU COLOR_MENU_BORDER "%s", m->top + line, m->left, l);
        if (item && item->label) {
            int cols = m->inner - 2;
            buf_puts(&b, line - 1 == m->sel ? BG_SELECTED : BG_MENU);
            buf_puts(&b, COLOR_TEXT "  ");
            cols -= buf_cols(&b, item->label, cols);
            while (cols-- > 0)
                buf_add(&b, " ", 1);
            buf_puts(&b, BG_MENU COLOR_MENU_BORDER);
        } else {
            for (int i = 0; i < m->inner; i++)
                buf_puts(&b, "─");
        }
        buf_puts(&b, r);
        buf_puts(&b, RESET);
    }
    buf_write(&b);
}

static void menu_step(Menu *m, int step)
{
    int i = m->sel;

    do
        i = (i + step + m->n) % m->n;
    while (m->items[i].action == ACT_SEPARATOR);
    m->sel = i;
}

/* Shows the menu at screen cell (x, y) until an item is picked or it is dismissed. */
static int run_menu(const MenuItem *items, int n, int x, int y)
{
    static const char hover_on[] = "\x1b[?1003h", hover_off[] = "\x1b[?1003l";
    Menu m = { .items = items, .n = n };
    unsigned char buf[512];
    int result = ACT_NONE;
    bool done = false;

    if (write(STDOUT_FILENO, hover_on, sizeof hover_on - 1) < 0)
        return ACT_NONE;
    while (!done) {
        place_menu(&m, x, y);
        draw(NULL, NULL);
        draw_menu(&m);
        int len = read_input(buf, sizeof buf);
        if (len < 0)
            exit(0);
        for (int i = 0; i < len && !done; i++) {
            if (buf[i] == '\r' || buf[i] == ' ') {
                result = items[m.sel].action;
                done = true;
            } else if (buf[i] == 'j' || buf[i] == 'k') {
                menu_step(&m, buf[i] == 'j' ? 1 : -1);
            } else if (buf[i] == 'q' || buf[i] == 0x03) {
                done = true;
            } else if (buf[i] == 0x1B) {
                int k = escape_len(buf + i, len - i);
                const unsigned char *seq = buf + i;
                unsigned char final = seq[k - 1];
                int bt, mx, my;
                i += k - 1;
                if (k == 1) {
                    done = true;
                } else if (k > 3 && seq[2] == '<' &&
                           sscanf((const char *)seq + 3, "%d;%d;%d", &bt, &mx, &my) == 3) {
                    int idx = my - m.top - 1;
                    bool in_box = mx > m.left && mx <= m.left + m.inner && idx >= 0 && idx < n;
                    bool usable = in_box && items[idx].action != ACT_SEPARATOR;
                    if (bt & 32) { /* motion */
                        if (usable)
                            m.sel = idx;
                    } else if (final == 'M' && (bt == 0 || bt == 2)) {
                        if (usable)
                            result = items[idx].action;
                        if (usable || !in_box)
                            done = true;
                    }
                } else if (final == 'A' || final == 'B') {
                    menu_step(&m, final == 'B' ? 1 : -1);
                }
            }
        }
    }
    if (write(STDOUT_FILENO, hover_off, sizeof hover_off - 1) < 0) {
        /* The terminal is gone. */
    }
    return result;
}

/* Actions ------------------------------------------------------------------------------------ */

static void run_action(int action, const Row *row)
{
    char dir[PATH_MAX];

    switch (action) {
    case ACT_OPEN:
        activate();
        break;
    case ACT_OPEN_SIDE:
        send_to_tcode("open-side", row->path);
        break;
    case ACT_PREVIEW:
        send_to_tcode("preview", row->path);
        break;
    case ACT_PROJECT:
        send_to_tcode("project", row->path);
        break;
    case ACT_COMPARE:
        send_to_tcode("compare", row->path);
        break;
    case ACT_HISTORY:
        send_to_tcode("history", row->path);
        break;
    case ACT_NEW_FILE:
    case ACT_NEW_FOLDER:
    case ACT_PASTE:
        if (row)
            target_dir(dir, sizeof dir);
        else
            snprintf(dir, sizeof dir, "%s", fm.root);
        if (action == ACT_PASTE)
            paste_into(dir);
        else
            create_in(dir, action == ACT_NEW_FOLDER);
        break;
    case ACT_TERMINAL:
        snprintf(dir, sizeof dir, "%s", row ? row->path : fm.root);
        if (row && !row->dir)
            *strrchr(dir, '/') = '\0';
        send_to_tcode("terminal", dir);
        break;
    case ACT_CUT:
    case ACT_COPY:
        clip_selected(action == ACT_CUT);
        break;
    case ACT_COPY_PATH:
        send_to_tcode("copy", row->path);
        break;
    case ACT_COPY_RELATIVE:
        send_to_tcode("copy", relative(row->path));
        break;
    case ACT_RENAME:
        rename_selected();
        break;
    case ACT_DELETE:
        delete_selected();
        break;
    }
}

/* The menu for row (-1: the empty space), opened at screen cell (x, y). */
void context_menu(int row, int x, int y)
{
    MenuItem items[MAX_ITEMS];
    const Row *r = NULL;
    char path[PATH_MAX];

    if (row >= 0) {
        fm.sel = row;
        r = &fm.rows[row];
    }
    int n = build_menu(r, items);
    int action = run_menu(items, n, x, y);
    if (r) { /* the action may rebuild the rows; keep what it applies to */
        snprintf(path, sizeof path, "%s", r->path);
        Row copy = *r;
        copy.path = path;
        copy.name = strrchr(path, '/') + 1;
        run_action(action, &copy);
    } else {
        run_action(action, NULL);
    }
}
