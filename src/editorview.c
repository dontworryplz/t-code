/*
 * What sits next to an editor terminal: a minimap of the file (like GNOME Text Editor's) and a
 * scrollbar. micro draws on the alternate screen, so the terminal has no scrollback of its own:
 * micro reports which lines it shows through the terminal title (micro/plug/tcode), and t-code
 * scrolls it through a named pipe that the plugin reads (TCODE_CONTROL):
 *   scroll <line>       show the file from that line (0-based) on
 *   command <command>   run a micro command
 * Lines with clangd's errors (red) and warnings (yellow), which micro reports in the title, are
 * marked across the whole minimap.
 * Over the terminal lie VS Code's indent guides: a line at each indentation level, and the one of
 * the brackets around the cursor in their color. They are worked out from the text on the screen,
 * which is never behind micro (the saved file can be).
 */
#define _DEFAULT_SOURCE /* O_CLOEXEC under -std=c17 */
#include "app.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MINIMAP_WIDTH 100
#define LINE_PITCH 3     /* pixels from one line of the minimap to the next */
#define LINE_HEIGHT 2
#define MAX_TEXT (4 << 20) /* bigger files get no minimap */
#define OWN_SCROLL_US 300000 /* reports this soon after our own scroll may predate it */
#define TERM_PADDING 1       /* the terminal's padding (css below), around its cells */

struct EditorView {
    Tab *tab;
    GtkWidget *minimap;
    GtkAdjustment *adjustment;
    gboolean updating;  /* setting the adjustment from a report, not from the user */
    gint64 scrolled_at; /* when t-code last scrolled the editor */
    int control_fd;
    char *control_path;
    GFileMonitor *monitor;
    guint reload_id;
    char *text;         /* the file, as last saved */
    gsize size;
    GArray *starts;     /* offset of each line in text */
    GArray *in_comment; /* gboolean: the line starts inside a block comment */
    gboolean c_like;    /* C/C++ source: colored; anything else is drawn plain */
    char *problems;     /* as micro reported them: "e12,w30", or "-" */
    GArray *problem_lines; /* Problem */
    double grab;        /* while dragging the viewport: where in it the pointer is */
    gboolean dragging;
    GtkWidget *guides;  /* drawn over the terminal */
    guint text_version; /* counts loads of text */
    struct {
        guint version;
        int first, last, line;
    } enclosing;        /* the last enclosing_line() */
};

typedef struct {
    int line; /* 0-based */
    gboolean error;
} Problem;

/* Colors ------------------------------------------------------------------------------------- */

typedef enum {
    INK_TEXT, INK_COMMENT, INK_STRING, INK_NUMBER, INK_KEYWORD, INK_TYPE, INK_STATEMENT, INK_FUNCTION,
    INK_CONSTANT, INK_PUNCTUATION, INK_COUNT
} Ink;

/* micro/colorschemes/dark-modern.micro: identifier, comment, constant.string, constant.number,
 * keyword (also macros and the name after #define), type, statement (also preprocessor
 * directives), identifier.func, identifier.constant, symbol. */
static const char *const ink_hex[INK_COUNT] = {
    "#9CDCFE", "#6A9955", "#CE9178", "#B5CEA8", "#569CD6", "#4EC9B0", "#C586C0", "#DCDCAA", "#4FC1FF", "#CCCCCC",
};
static GdkRGBA inks[INK_COUNT], plain_ink, error_ink, warning_ink;

static const char *const statements[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "goto", "try", "catch", "throw", "co_return", "co_await", "co_yield", NULL,
};
static const char *const keywords[] = {
    "int", "char", "void", "short", "long", "float", "double", "signed", "unsigned", "bool", "auto",
    "const", "static", "extern", "inline", "volatile", "register", "struct", "union", "enum",
    "typedef", "sizeof", "class", "namespace", "template", "typename", "using", "public", "private",
    "protected", "virtual", "override", "constexpr", "consteval", "constinit", "noexcept", "new",
    "delete", "this", "true", "false", "nullptr", "NULL", "operator", "friend", "explicit", "mutable",
    "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast", "decltype", "concept", "requires",
    "TRUE", "FALSE", NULL,
};
/* Lowercase type names micro/syntax/c.yaml colors as types (besides foo_t). */
static const char *const types[] = {
    "FILE", "DIR", "va_list", "jmp_buf", "fd_set", "gboolean", "gchar", "guchar", "gint", "guint", "gshort",
    "gushort", "glong", "gulong", "gint8", "guint8", "gint16", "guint16", "gint32", "guint32", "gint64",
    "guint64", "gfloat", "gdouble", "gsize", "gssize", "goffset", "gintptr", "guintptr", "gpointer",
    "gconstpointer", "gunichar", "gquark", NULL,
};

static gboolean in_list(const char *const *list, const char *word, int len)
{
    for (int i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == len && strncmp(list[i], word, len) == 0)
            return TRUE;
    return FALSE;
}

static gboolean is_word_char(char c)
{
    return g_ascii_isalnum(c) || c == '_';
}

/* Skips a block comment from i to just after its end, or to the end of the line (*open = TRUE). */
static int comment_end(const char *s, int len, int i, gboolean *open)
{
    while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
        i++;
    *open = i + 1 >= len;
    return *open ? len : i + 2;
}

/* A word's color, following micro/syntax/c.yaml: ALL_CAPS names are constants (macros when
 * called), PascalCase names and foo_t are types, and a name before "(" is a function. */
static Ink word_ink(const char *s, int len, int start, int end)
{
    const char *word = s + start;
    int n = end - start, next = end;
    gboolean upper = FALSE, lower = FALSE;

    if (in_list(statements, word, n))
        return INK_STATEMENT;
    if (in_list(keywords, word, n))
        return INK_KEYWORD;
    if (in_list(types, word, n) || (n > 2 && word[n - 2] == '_' && word[n - 1] == 't'))
        return INK_TYPE;
    while (next < len && (s[next] == ' ' || s[next] == '\t'))
        next++;
    for (int i = 0; i < n; i++) {
        upper |= g_ascii_isupper(word[i]);
        lower |= g_ascii_islower(word[i]);
    }
    if (upper && !lower && n > 1)
        return next < len && s[next] == '(' ? INK_KEYWORD : INK_CONSTANT;
    if (next < len && s[next] == '(')
        return INK_FUNCTION;
    return g_ascii_isupper(word[0]) && lower ? INK_TYPE : INK_TEXT;
}

/* Colors one line, calling paint for each run of visible characters. Returns whether the line
 * ends inside a block comment. */
typedef void (*PaintFunc)(cairo_t *cr, int column, int length, Ink ink);

static gboolean lex_line(const char *s, int len, gboolean comment, cairo_t *cr, PaintFunc paint)
{
    int i = 0, column = 0, k = 0;
    gboolean include = FALSE, name_next = FALSE; /* after #include; the name after #define */

    while (k < len && (s[k] == ' ' || s[k] == '\t'))
        k++;
    while (i < len) {
        int start = i;
        Ink ink = INK_TEXT;
        char c = s[i];

        if (comment) {
            i = comment_end(s, len, i, &comment);
            ink = INK_COMMENT;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            i = len;
            ink = INK_COMMENT;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i = comment_end(s, len, i + 2, &comment);
            ink = INK_COMMENT;
        } else if (c == '"' || c == '\'') {
            for (i++; i < len && s[i] != c; i++)
                if (s[i] == '\\')
                    i++;
            i = MIN(i + 1, len);
            ink = INK_STRING;
        } else if (g_ascii_isdigit(c)) {
            while (i < len && (is_word_char(s[i]) || s[i] == '.'))
                i++;
            ink = INK_NUMBER;
        } else if (c == '#' && i == k && !comment) {
            /* A directive: "#include <a.h>" (the header like a string), "#define NAME" (the name
             * blue), "#if COND" (the condition blue). */
            int word;
            for (i++; i < len && (s[i] == ' ' || s[i] == '\t'); i++)
                ;
            for (word = i; i < len && is_word_char(s[i]); i++)
                ;
            include = i - word == 7 && strncmp(s + word, "include", 7) == 0;
            name_next = in_list((const char *const[]){ "define", "undef", "ifdef", "ifndef", NULL }, s + word, i - word);
            if (cr)
                paint(cr, column, i - start, INK_STATEMENT);
            column += i - start;
            if (in_list((const char *const[]){ "if", "elif", NULL }, s + word, i - word)) {
                while (i < len && (s[i] == ' ' || s[i] == '\t'))
                    i++, column++;
                start = i;
                while (i < len && !(s[i] == '/' && i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '*')))
                    i++;
                if (cr && i > start)
                    paint(cr, column, i - start, INK_KEYWORD);
                column += i - start;
            }
            continue;
        } else if (c == '<' && include) {
            while (i < len && s[i] != '>')
                i++;
            i = MIN(i + 1, len);
            ink = INK_STRING;
        } else if (is_word_char(c)) {
            while (i < len && is_word_char(s[i]))
                i++;
            ink = name_next ? INK_KEYWORD : word_ink(s, len, start, i);
            name_next = FALSE;
        } else if (c == ' ') {
            i++;
            column++;
            continue;
        } else if (c == '\t') {
            i++;
            column += 4 - column % 4;
            continue;
        } else {
            i++;
            ink = INK_PUNCTUATION;
        }
        /* A run with spaces inside (a comment, a string) is still one bar, like the text. */
        if (cr)
            paint(cr, column, i - start, ink);
        column += i - start;
    }
    return comment;
}

/* The file ----------------------------------------------------------------------------------- */

static void clear_text(EditorView *v)
{
    g_clear_pointer(&v->text, g_free);
    v->size = 0;
    g_array_set_size(v->starts, 0);
    g_array_set_size(v->in_comment, 0);
}

static void load_text(EditorView *v)
{
    char *text = NULL;
    gsize size = 0;
    gboolean comment = FALSE;

    clear_text(v);
    v->text_version++;
    if (!v->tab->path || !g_file_get_contents(v->tab->path, &text, &size, NULL) || size > MAX_TEXT) {
        g_free(text);
        gtk_widget_queue_draw(v->minimap);
        return;
    }
    v->text = text;
    v->size = size;
    for (gsize start = 0; start <= size;) {
        const char *newline = memchr(text + start, '\n', size - start);
        gsize end = newline ? (gsize)(newline - text) : size;
        g_array_append_val(v->starts, start);
        g_array_append_val(v->in_comment, comment);
        if (v->c_like)
            comment = lex_line(text + start, end - start, comment, NULL, NULL);
        if (!newline)
            break;
        start = end + 1;
    }
    gtk_widget_queue_draw(v->minimap);
}

static gboolean reload_now(gpointer data)
{
    EditorView *v = data;

    v->reload_id = 0;
    load_text(v);
    return G_SOURCE_REMOVE;
}

static void on_file_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED,
                            GFile *other G_GNUC_UNUSED, GFileMonitorEvent event G_GNUC_UNUSED, EditorView *v)
{
    if (!v->reload_id)
        v->reload_id = g_timeout_add(200, reload_now, v);
}

/* The lexer knows C and C++ only: other files (Makefiles, scripts, Markdown) would get '#'
 * comments drawn as preprocessor lines and apostrophes as unclosed character literals. */
static gboolean is_c_like(const char *path)
{
    static const char *const extensions[] = {
        ".c", ".h", ".cc", ".cpp", ".cxx", ".c++", ".hh", ".hpp", ".hxx", ".h++", ".inl", ".ipp", ".tpp", NULL,
    };
    const char *dot = path ? strrchr(path, '.') : NULL;

    for (int i = 0; dot && extensions[i]; i++)
        if (g_ascii_strcasecmp(dot, extensions[i]) == 0)
            return TRUE;
    return FALSE;
}

/* Follows tab->path: call again when it changes. */
void editor_view_set_path(Tab *tab)
{
    EditorView *v = tab->view;
    GFile *file;

    if (!v)
        return;
    v->c_like = is_c_like(tab->path);
    if (v->monitor) {
        g_file_monitor_cancel(v->monitor);
        g_clear_object(&v->monitor);
    }
    file = g_file_new_for_path(tab->path);
    v->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (v->monitor)
        g_signal_connect(v->monitor, "changed", G_CALLBACK(on_file_changed), v);
    g_object_unref(file);
    load_text(v);
}

/* Talking to the editor ------------------------------------------------------------------------- */

/* Sends one line to the plugin; FALSE when there is no pipe (an older plugin, or it is full). */
gboolean editor_control(Tab *tab, const char *line)
{
    EditorView *v = tab->view;
    char *data;
    gboolean ok;

    if (!v || v->control_fd < 0)
        return FALSE;
    data = g_strdup_printf("%s\n", line);
    ok = write(v->control_fd, data, strlen(data)) == (ssize_t)strlen(data);
    g_free(data);
    return ok;
}

static void scroll_editor(EditorView *v, int top)
{
    Tab *tab = v->tab;
    int most = MAX(0, tab->lines - tab->height);
    char *line;

    top = CLAMP(top, 0, most);
    if (top == tab->top)
        return;
    tab->top = top;
    v->scrolled_at = g_get_monotonic_time();
    line = g_strdup_printf("scroll %d", top);
    editor_control(tab, line);
    g_free(line);
    v->updating = TRUE;
    gtk_adjustment_set_value(v->adjustment, top);
    v->updating = FALSE;
    gtk_widget_queue_draw(v->minimap);
}

/* Takes what micro reported (tab->top, height, lines) into the scrollbar and the minimap. */
void editor_view_update(Tab *tab, int top)
{
    EditorView *v = tab->view;

    if (!v)
        return;
    /* While t-code itself scrolls, reports from before that would pull the view back. */
    if (g_get_monotonic_time() - v->scrolled_at > OWN_SCROLL_US)
        tab->top = top;
    v->updating = TRUE;
    gtk_adjustment_configure(v->adjustment, tab->top, 0, MAX(MAX(tab->lines, tab->top + tab->height), 1),
                             1, MAX(tab->height - 1, 1), MAX(tab->height, 1));
    v->updating = FALSE;
    gtk_widget_queue_draw(v->minimap);
    gtk_widget_queue_draw(v->guides);
}

static void on_scrollbar(GtkAdjustment *adjustment, EditorView *v)
{
    if (!v->updating)
        scroll_editor(v, (int)(gtk_adjustment_get_value(adjustment) + 0.5));
}

/* The minimap -------------------------------------------------------------------------------- */

/* Where the minimap starts (in pixels of the whole file), so that the shown lines stay in view:
 * all of it while it fits, else it moves along with the editor like VS Code's. */
static double minimap_offset(EditorView *v, int area)
{
    Tab *tab = v->tab;
    double whole = (double)tab->lines * LINE_PITCH;
    int most = MAX(1, tab->lines - tab->height);

    if (whole <= area)
        return 0;
    return MIN(1.0, (double)tab->top / most) * (whole - area);
}

/* The line at the top of the editor that puts the viewport's top at y. */
static int top_for_viewport(EditorView *v, double y, int area)
{
    Tab *tab = v->tab;
    double whole = (double)tab->lines * LINE_PITCH;
    int most = MAX(1, tab->lines - tab->height);
    double per_line = whole <= area ? LINE_PITCH : (area - (double)tab->height * LINE_PITCH) / most;

    if (per_line <= 0) /* the viewport is as tall as the minimap: go by proportion */
        return (int)(y / area * most);
    return (int)(y / per_line + 0.5);
}

static void paint_run(cairo_t *cr, int column, int length, Ink ink)
{
    if (column >= MINIMAP_WIDTH)
        return;
    gdk_cairo_set_source_rgba(cr, ink == INK_COUNT ? &plain_ink : &inks[ink]);
    cairo_rectangle(cr, column, 0, MIN(length, MINIMAP_WIDTH - column), LINE_HEIGHT);
    cairo_fill(cr);
}

static void paint_plain(cairo_t *cr, int column, int length, Ink ink G_GNUC_UNUSED)
{
    paint_run(cr, column, length, INK_COUNT);
}

static void draw_minimap(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width, int height, gpointer data)
{
    EditorView *v = data;
    Tab *tab = v->tab;
    double offset = minimap_offset(v, height);
    int first = (int)(offset / LINE_PITCH), n = (int)v->starts->len;

    for (int i = first; v->text && i < n && (i - first) * LINE_PITCH < height + LINE_PITCH; i++) {
        gsize start = g_array_index(v->starts, gsize, i);
        gsize end = i + 1 < n ? g_array_index(v->starts, gsize, i + 1) - 1 : v->size;
        cairo_save(cr);
        cairo_translate(cr, 2, i * LINE_PITCH - offset);
        lex_line(v->text + start, end - start, g_array_index(v->in_comment, gboolean, i), cr,
                 tab->app->plain || !v->c_like ? paint_plain : paint_run);
        cairo_restore(cr);
    }
    /* Problem lines: the whole width, over the text. */
    for (guint k = 0; k < v->problem_lines->len; k++) {
        Problem *p = &g_array_index(v->problem_lines, Problem, k);
        double y = p->line * LINE_PITCH - offset;
        if (y < -LINE_PITCH || y > height)
            continue;
        gdk_cairo_set_source_rgba(cr, p->error ? &error_ink : &warning_ink);
        cairo_rectangle(cr, 0, y, width, LINE_PITCH);
        cairo_fill(cr);
    }
    if (tab->lines > 0 && tab->height > 0) {
        cairo_set_source_rgba(cr, 1, 1, 1, v->dragging ? 0.14 : 0.08);
        cairo_rectangle(cr, 0, tab->top * LINE_PITCH - offset, width, tab->height * LINE_PITCH);
        cairo_fill(cr);
    }
}

static void on_drag_begin(GtkGestureDrag *gesture G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, EditorView *v)
{
    Tab *tab = v->tab;
    int area = gtk_widget_get_height(v->minimap);
    double offset = minimap_offset(v, area);
    double view_y = tab->top * LINE_PITCH - offset, view_h = tab->height * LINE_PITCH;

    /* A click outside the viewport centers it there; then dragging moves it. */
    if (y < view_y || y > view_y + view_h) {
        int line = (int)((y + offset) / LINE_PITCH);
        scroll_editor(v, line - tab->height / 2);
        view_y = tab->top * LINE_PITCH - minimap_offset(v, area);
    }
    v->grab = y - view_y;
    v->dragging = TRUE;
    gtk_widget_grab_focus(GTK_WIDGET(tab->term));
}

static void on_drag_update(GtkGestureDrag *gesture, double dx G_GNUC_UNUSED, double dy, EditorView *v)
{
    double x, y;

    gtk_gesture_drag_get_start_point(gesture, &x, &y);
    scroll_editor(v, top_for_viewport(v, y + dy - v->grab, gtk_widget_get_height(v->minimap)));
}

static void on_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED, double dx G_GNUC_UNUSED, double dy G_GNUC_UNUSED,
                        EditorView *v)
{
    v->dragging = FALSE;
    gtk_widget_queue_draw(v->minimap);
}

static gboolean on_minimap_wheel(GtkEventControllerScroll *scroll G_GNUC_UNUSED, double dx G_GNUC_UNUSED,
                                 double dy, EditorView *v)
{
    scroll_editor(v, v->tab->top + (int)(dy * 3));
    return TRUE;
}

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

static gboolean is_opening(char c)
{
    return c == '(' || c == '[' || c == '{';
}

typedef struct {
    GArray *brackets;
    int row;
} RowScan;

static void add_bracket(gpointer data, int offset, char c)
{
    RowScan *scan = data;
    Bracket b = { scan->row, offset, c };

    g_array_append_val(scan->brackets, b);
}

typedef struct {
    GArray *stack; /* lines of the open brackets */
    int line;
} FileScan;

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

static void draw_guide(cairo_t *cr, const GdkRGBA *ink, int gutter, int column, int row, double cw, double ch)
{
    gdk_cairo_set_source_rgba(cr, ink);
    cairo_rectangle(cr, (int)(TERM_PADDING + (gutter + column) * cw), TERM_PADDING + row * ch, 1, ch);
    cairo_fill(cr);
}

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

static void redraw_guides(VteTerminal *term G_GNUC_UNUSED, EditorView *v)
{
    gtk_widget_queue_draw(v->guides);
}

/* Setting up --------------------------------------------------------------------------------- */

static const char *const css =
    ".tcode-editor scrollbar { background: #1f1f1f; border: none; }"
    ".tcode-editor scrollbar slider { min-width: 6px; margin: 2px; border-radius: 6px; background: rgba(121, 121, 121, .4); }"
    ".tcode-editor scrollbar slider:hover { background: rgba(100, 100, 100, .7); }"
    ".tcode-editor .minimap { background: #1f1f1f; }"
    ".tcode-editor vte-terminal { padding: 1px; }"; /* TERM_PADDING */

/* Builds the editor's page around term: [ terminal (under the guides) | minimap | scrollbar ]. */
GtkWidget *editor_page(VteTerminal *term)
{
    static gboolean styled;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *overlay;

    if (!styled) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        for (int i = 0; i < INK_COUNT; i++) {
            gdk_rgba_parse(&inks[i], ink_hex[i]);
            inks[i].alpha = 0.75;
        }
        gdk_rgba_parse(&plain_ink, "#cccccc");
        plain_ink.alpha = 0.45;
        gdk_rgba_parse(&error_ink, "#f14c4c");   /* gutter-error */
        gdk_rgba_parse(&warning_ink, "#cca700"); /* gutter-warning */
        gdk_rgba_parse(&guide_ink, "#404040");
        gdk_rgba_parse(&active_ink, "#707070");
        gdk_rgba_parse(&pair_inks[0], "#ffd700");
        gdk_rgba_parse(&pair_inks[1], "#da70d6");
        gdk_rgba_parse(&pair_inks[2], "#179fff");
        styled = TRUE;
    }
    gtk_widget_add_css_class(page, "tcode-editor");
    gtk_widget_set_hexpand(GTK_WIDGET(term), TRUE);
    overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(term));
    gtk_box_append(GTK_BOX(page), overlay);
    return page;
}

/* Adds the minimap, the scrollbar and the control pipe to an editor tab (its page from
 * editor_page). Returns the pipe's path for TCODE_CONTROL, or NULL. */
const char *editor_view_init(Tab *tab, int number)
{
    EditorView *v = g_new0(EditorView, 1);
    GtkGesture *drag = gtk_gesture_drag_new();
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkWidget *scrollbar;

    tab->view = v;
    v->tab = tab;
    v->control_fd = -1;
    v->starts = g_array_new(FALSE, FALSE, sizeof(gsize));
    v->in_comment = g_array_new(FALSE, FALSE, sizeof(gboolean));
    v->problem_lines = g_array_new(FALSE, FALSE, sizeof(Problem));

    v->guides = gtk_drawing_area_new();
    gtk_widget_set_can_target(v->guides, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->guides), draw_guides, v, NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(gtk_widget_get_parent(GTK_WIDGET(tab->term))), v->guides);
    g_signal_connect(tab->term, "contents-changed", G_CALLBACK(redraw_guides), v);

    v->minimap = gtk_drawing_area_new();
    gtk_widget_add_css_class(v->minimap, "minimap");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->minimap), MINIMAP_WIDTH + 4);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->minimap), draw_minimap, v, NULL);
    gtk_widget_set_visible(v->minimap, tab->app->settings.minimap);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
    gtk_widget_add_controller(v->minimap, GTK_EVENT_CONTROLLER(drag));
    g_signal_connect(wheel, "scroll", G_CALLBACK(on_minimap_wheel), v);
    gtk_widget_add_controller(v->minimap, wheel);
    gtk_box_append(GTK_BOX(tab->page), v->minimap);

    v->adjustment = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    g_signal_connect(v->adjustment, "value-changed", G_CALLBACK(on_scrollbar), v);
    scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->adjustment);
    gtk_box_append(GTK_BOX(tab->page), scrollbar);

    /* O_RDWR keeps the pipe open with no reader yet, and lets writes wait in it until micro reads. */
    v->control_path = g_strdup_printf("%s/t-code-%d-editor-%d", g_get_user_runtime_dir(), getpid(), number);
    unlink(v->control_path);
    if (mkfifo(v->control_path, 0600) == 0)
        v->control_fd = open(v->control_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (v->control_fd < 0) {
        g_warning("t-code: no control pipe for %s: %s", tab->path, g_strerror(errno));
        unlink(v->control_path);
        g_clear_pointer(&v->control_path, g_free);
    }
    editor_view_set_path(tab);
    return v->control_path;
}

/* micro's problem lines ("e12,w30", or "-" for none) for the minimap. */
void editor_view_set_problems(Tab *tab, const char *spec)
{
    EditorView *v = tab->view;
    char **marks;

    if (!v || g_strcmp0(v->problems, spec) == 0)
        return;
    g_set_str(&v->problems, spec);
    g_array_set_size(v->problem_lines, 0);
    marks = g_strsplit(spec, ",", -1);
    for (int i = 0; marks[i]; i++) {
        int line = atoi(marks[i] + (marks[i][0] ? 1 : 0));
        if ((marks[i][0] == 'e' || marks[i][0] == 'w') && line > 0)
            g_array_append_val(v->problem_lines, ((Problem){ line - 1, marks[i][0] == 'e' }));
    }
    g_strfreev(marks);
    gtk_widget_queue_draw(v->minimap);
}

void editor_view_show_minimap(Tab *tab, gboolean show)
{
    if (tab->view)
        gtk_widget_set_visible(tab->view->minimap, show);
}

void editor_view_redraw(Tab *tab)
{
    if (tab->view) {
        gtk_widget_queue_draw(tab->view->minimap);
        gtk_widget_queue_draw(tab->view->guides);
    }
}

/* Closing the pipe ends the plugin's reader (cat). */
void editor_view_free(EditorView *v)
{
    if (!v)
        return;
    if (v->control_fd >= 0)
        close(v->control_fd);
    if (v->control_path) {
        unlink(v->control_path);
        g_free(v->control_path);
    }
    if (v->monitor) {
        g_file_monitor_cancel(v->monitor);
        g_object_unref(v->monitor);
    }
    if (v->reload_id)
        g_source_remove(v->reload_id);
    g_free(v->text);
    g_array_free(v->starts, TRUE);
    g_array_free(v->in_comment, TRUE);
    g_array_free(v->problem_lines, TRUE);
    g_free(v->problems);
    g_free(v);
}
