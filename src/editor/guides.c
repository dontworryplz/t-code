/*
 * VS Code's indent guides over an editor's terminal (see guides.h): a line at each indentation
 * level, and the one of the brackets around the cursor in their color. They are worked out from
 * the text on the screen, which is never behind micro (the saved file can be); only when both
 * brackets of the block around the cursor are off the screen is the saved file scanned.
 */
#include "editor/guides.h"

#include "editor/editorview.h"

#include <stdlib.h>
#include <string.h>

/* Indent guides ------------------------------------------------------------------------------ */

/* Dark Modern: editorIndentGuide.background1, .activeBackground1 (without code coloring) and
 * editorBracketHighlight.foreground1..3, the bracket pair colors by depth. */
static GdkRGBA guide_ink, active_ink, pair_inks[3];

typedef struct {
    int line;         /* the file's line (1-based) in the row; 0 for none (past the end, wrapped) */
    const char *text; /* after the gutter */
    int len;
    int indent;       /* columns of leading spaces; -1 for a blank row */
} Row;

typedef struct {
    int row, offset; /* offset in the row's text */
    char c;
} Bracket;

typedef void (*BracketFunc)(gpointer data, int offset, char c);

/* Calls found for each bracket outside strings and comments. Returns whether the line ends inside
 * a block comment. */
static gboolean scan_brackets(const char *s, int len, gboolean comment, BracketFunc found, gpointer data)
{
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (comment) {
            if (c == '*' && i + 1 < len && s[i + 1] == '/') {
                comment = FALSE;
                i++;
            }
        } else if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            break;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            comment = TRUE;
            i++;
        } else if (c == '"' || c == '\'') {
            for (i++; i < len && s[i] != c; i++)
                if (s[i] == '\\')
                    i++;
        } else if (c && strchr("([{)]}", c)) {
            found(data, i, c);
        }
    }
    return comment;
}

/* (, [ or {. */
static gboolean is_opening(char c)
{
    return c == '(' || c == '[' || c == '{';
}

/* Collecting the brackets of the rows on the screen, in order (add_bracket). */
typedef struct {
    GArray *brackets;
    int row;
} RowScan;

/* BracketFunc: one more bracket of the row being scanned. */
static void add_bracket(gpointer data, int offset, char c)
{
    RowScan *scan = data;
    Bracket b = { scan->row, offset, c };

    g_array_append_val(scan->brackets, b);
}

/* Following the open brackets through the saved file (track_bracket). */
typedef struct {
    GArray *stack; /* lines of the open brackets */
    int line;
} FileScan;

/* BracketFunc: an opening bracket goes on the stack, a closing one takes the last off. */
static void track_bracket(gpointer data, int offset G_GNUC_UNUSED, char c)
{
    FileScan *scan = data;

    if (is_opening(c))
        g_array_append_val(scan->stack, scan->line);
    else if (scan->stack->len > 0)
        g_array_set_size(scan->stack, scan->stack->len - 1);
}

/* The line (0-based) of the innermost bracket in the saved file that opens before line first and
 * closes after line last, or -1: the block around the cursor when neither end is on the screen. */
static int enclosing_line(EditorView *v, int first, int last)
{
    FileScan scan = { g_array_new(FALSE, FALSE, sizeof(int)), 0 };
    int n = (int)v->starts->len, low = 0;
    gboolean comment = FALSE;

    if (v->enclosing.version == v->text_version && v->enclosing.first == first && v->enclosing.last == last)
        return v->enclosing.line;
    for (; v->text && scan.line <= last && scan.line < n; scan.line++) {
        gsize start = g_array_index(v->starts, gsize, scan.line);
        gsize end = scan.line + 1 < n ? g_array_index(v->starts, gsize, scan.line + 1) - 1 : v->size;
        if (scan.line == first)
            low = (int)scan.stack->len;
        comment = scan_brackets(v->text + start, (int)(end - start), comment, track_bracket, &scan);
        if (scan.line >= first)
            low = MIN(low, (int)scan.stack->len);
    }
    v->enclosing.version = v->text_version;
    v->enclosing.first = first;
    v->enclosing.last = last;
    v->enclosing.line = scan.line > last && low > 0 ? g_array_index(scan.stack, int, low - 1) : -1;
    g_array_free(scan.stack, TRUE);
    return v->enclosing.line;
}

/* Columns of leading blanks of a line of the file, -1 when blank. */
static int file_indent(EditorView *v, int line, int tab_size)
{
    int n = (int)v->starts->len, column = 0;
    gsize i, end;

    if (line < 0 || line >= n)
        return -1;
    end = line + 1 < n ? g_array_index(v->starts, gsize, line + 1) - 1 : v->size;
    for (i = g_array_index(v->starts, gsize, line); i < end && (v->text[i] == ' ' || v->text[i] == '\t'); i++)
        column = v->text[i] == '\t' ? column + tab_size - column % tab_size : column + 1;
    return i < end ? column : -1;
}

/* Splits the screen into rows of the file: "<gutter> <line number> <text>", where the gutter holds
 * micro's diagnostics and diff marks. Returns the gutter's width in columns (the most common one,
 * as a wrapped row has no line number), or -1. */
static int parse_rows(char **lines, Row *rows, int n)
{
    int starts[n], count[32] = { 0 }, best = -1;

    for (int r = 0; r < n; r++) {
        const char *s = lines[r];
        int i = 0, digits;

        rows[r] = (Row){ 0, "", 0, -1 };
        starts[r] = -1;
        while (s[i] && !g_ascii_isdigit(s[i]) && i < 16)
            i++;
        for (digits = i; g_ascii_isdigit(s[i]); i++)
            ;
        if (i == digits || (s[i] && s[i] != ' '))
            continue;
        rows[r].line = atoi(s + digits);
        rows[r].text = s[i] ? s + i + 1 : s + i;
        rows[r].len = (int)strlen(rows[r].text);
        starts[r] = (int)g_utf8_strlen(s, i) + 1;
        if (starts[r] < 32 && ++count[starts[r]] > (best < 0 ? 0 : count[best]))
            best = starts[r];
    }
    for (int r = 0; r < n; r++) {
        int k = 0;
        if (starts[r] != best) {
            rows[r].line = 0;
            continue;
        }
        while (k < rows[r].len && rows[r].text[k] == ' ')
            k++;
        rows[r].indent = k < rows[r].len ? k : -1;
    }
    return best;
}

/* Guides on a blank row follow the rows around it, like VS Code's. */
static int guide_count(Row *rows, int n, int r, int tab_size)
{
    int above = -1, below = -1;

    if (rows[r].indent >= 0)
        return (rows[r].indent + tab_size - 1) / tab_size;
    for (int i = r - 1; i >= 0 && above < 0; i--)
        if (rows[i].line > 0)
            above = rows[i].indent;
    for (int i = r + 1; i < n && below < 0; i++)
        if (rows[i].line > 0)
            below = rows[i].indent;
    if (above < 0 && below < 0)
        return 0;
    if (above < 0 || below < 0)
        return (MAX(above, below) + tab_size - 1) / tab_size;
    if (above == below)
        return (above + tab_size - 1) / tab_size;
    return 1 + MIN(above, below) / tab_size; /* inside the block one of them opens or closes */
}

/* Byte offset of a column in a row's text; past its end, offsets go on one per column. */
static int column_offset(const Row *row, int column)
{
    const char *p = row->text, *end = row->text + row->len;

    for (; column > 0 && p < end; column--)
        p = g_utf8_next_char(p);
    return (int)(p - row->text) + column;
}

/* The rows of the innermost brackets around the cursor that are on different lines with lines in
 * between (-1: above the screen, n: below it), and the column of their guide. FALSE when there are
 * none, or the cursor's line is not on the screen. */
static gboolean active_pair(EditorView *v, Row *rows, int n, int *open, int *close, int *column)
{
    GArray *brackets = g_array_new(FALSE, FALSE, sizeof(Bracket));
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *pairs = g_array_new(FALSE, FALSE, sizeof(int) * 2);
    RowScan scan = { brackets, 0 };
    gboolean comment = FALSE, found = FALSE;
    int cursor_row = 0, cursor_offset, depth = 0, i = 0;

    /* As micro reports it: on the alternate screen, the terminal's cursor row is not the row on
     * the screen. */
    while (cursor_row < n && rows[cursor_row].line != v->tab->line)
        cursor_row++;
    if (cursor_row == n)
        goto done;
    cursor_offset = column_offset(&rows[cursor_row], v->tab->visual_col);
    for (scan.row = 0; scan.row < n; scan.row++)
        if (rows[scan.row].line > 0)
            comment = scan_brackets(rows[scan.row].text, rows[scan.row].len, comment, add_bracket, &scan);

    /* Before the cursor, the stack ends up with the open brackets around it. */
    for (; i < (int)brackets->len; i++) {
        Bracket *b = &g_array_index(brackets, Bracket, i);
        if (b->row > cursor_row || (b->row == cursor_row && b->offset >= cursor_offset))
            break;
        if (is_opening(b->c))
            g_array_append_val(stack, i);
        else if (stack->len > 0)
            g_array_set_size(stack, stack->len - 1);
    }
    /* After it, closing brackets at depth 0 close them, innermost first, then ones above. */
    for (; i < (int)brackets->len; i++) {
        Bracket *b = &g_array_index(brackets, Bracket, i);
        if (is_opening(b->c)) {
            depth++;
        } else if (depth > 0) {
            depth--;
        } else {
            int pair[2] = { -1, b->row };
            if (stack->len > 0) {
                pair[0] = g_array_index(brackets, Bracket, g_array_index(stack, int, stack->len - 1)).row;
                g_array_set_size(stack, stack->len - 1);
            }
            g_array_append_val(pairs, pair);
        }
    }
    /* Open brackets still on the stack close below the screen. (Then no pair opened above it: a
     * closing bracket on the screen would have closed them first.) */
    for (int k = (int)stack->len - 1; k >= 0; k--) {
        int pair[2] = { g_array_index(brackets, Bracket, g_array_index(stack, int, k)).row, n };
        g_array_append_val(pairs, pair);
    }
    for (guint k = 0; k < pairs->len && !found; k++) {
        int *pair = &g_array_index(pairs, int, 2 * k);
        if (pair[1] - pair[0] < 2)
            continue;
        *open = pair[0];
        *close = pair[1];
        *column = MIN(pair[0] >= 0 ? rows[pair[0]].indent : G_MAXINT, pair[1] < n ? rows[pair[1]].indent : G_MAXINT);
        found = *column >= 0;
    }
    if (!found) { /* both ends of the block are off the screen: look in the saved file */
        int first = -1, last = -1;
        for (int r = 0; r < n; r++)
            if (rows[r].line > 0) {
                first = first < 0 ? rows[r].line - 1 : first;
                last = rows[r].line - 1;
            }
        int line = first >= 0 ? enclosing_line(v, first, last) : -1;
        *open = -1;
        *close = n;
        *column = line >= 0 ? file_indent(v, line, v->tab->tab_size) : -1;
        found = *column >= 0;
    }
done:
    g_array_free(pairs, TRUE);
    g_array_free(stack, TRUE);
    g_array_free(brackets, TRUE);
    return found;
}

/* One guide: a 1 pixel line through row r at a column of the text (after the gutter). */
static void draw_guide(cairo_t *cr, const GdkRGBA *ink, int gutter, int column, int row, double cw, double ch)
{
    gdk_cairo_set_source_rgba(cr, ink);
    cairo_rectangle(cr, (int)(TERM_PADDING + (gutter + column) * cw), TERM_PADDING + row * ch, 1, ch);
    cairo_fill(cr);
}

/* Draws the guides of the rows on the screen, then the one of the block around the cursor in
 * its bracket pair color. */
static void draw_guides(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width G_GNUC_UNUSED,
                        int height G_GNUC_UNUSED, gpointer data)
{
    EditorView *v = data;
    Tab *tab = v->tab;
    int ts = MAX(tab->tab_size, 1), n, gutter, open, close, column;
    double cw = vte_terminal_get_char_width(tab->term), ch = vte_terminal_get_char_height(tab->term);
    char *text, **lines;

    /* Scrolled sideways, the leading blanks on the screen are not the indentation. */
    if (!tab->app->settings.guides || tab->height <= 0 || tab->start_col > 0)
        return;
    text = vte_terminal_get_text_format(tab->term, VTE_FORMAT_TEXT);
    if (!text)
        return;
    lines = g_strsplit(text, "\n", -1);
    n = MIN((int)g_strv_length(lines), tab->height);
    if (n > 0) {
        Row rows[n];
        gutter = parse_rows(lines, rows, n);
        for (int r = 0; gutter >= 0 && r < n; r++)
            for (int k = 0, count = rows[r].line > 0 ? guide_count(rows, n, r, ts) : 0; k < count; k++)
                draw_guide(cr, &guide_ink, gutter, k * ts, r, cw, ch);
        if (gutter >= 0 && active_pair(v, rows, n, &open, &close, &column)) {
            const GdkRGBA *ink = tab->app->plain ? &active_ink : &pair_inks[column / ts % 3];
            for (int r = MAX(open + 1, 0); r < MIN(close, n); r++)
                if (rows[r].line > 0 && (rows[r].indent < 0 || rows[r].indent > column))
                    draw_guide(cr, ink, gutter, column, r, cw, ch);
        }
    }
    g_strfreev(lines);
    g_free(text);
}

/* The terminal's text changed (micro redrew): the guides follow. */
static void redraw_guides(VteTerminal *term G_GNUC_UNUSED, EditorView *v)
{
    gtk_widget_queue_draw(v->guides);
}

void guides_init_colors(void)
{
    gdk_rgba_parse(&guide_ink, "#404040");
    gdk_rgba_parse(&active_ink, "#707070");
    gdk_rgba_parse(&pair_inks[0], "#ffd700");
    gdk_rgba_parse(&pair_inks[1], "#da70d6");
    gdk_rgba_parse(&pair_inks[2], "#179fff");
}

void guides_attach(EditorView *v, GtkOverlay *overlay)
{
    v->guides = gtk_drawing_area_new();
    gtk_widget_set_can_target(v->guides, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->guides), draw_guides, v, NULL);
    gtk_overlay_add_overlay(overlay, v->guides);
    g_signal_connect(v->tab->term, "contents-changed", G_CALLBACK(redraw_guides), v);
}
