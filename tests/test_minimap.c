/* Tests for the minimap in src/editor/minimap.c and its lexer (src/editor/lexer.c): the colors the
 * lexer gives C code, micro's problem lines and how they are drawn. The editor page's files are
 * included, to reach their static functions. */
#include "editor/control.c"
#include "editor/editorview.c"
#include "editor/guides.c"
#include "editor/lexer.c"
#include "editor/minimap.c"

#include <glib/gstdio.h>

#define COLUMNS 120

static Ink painted[COLUMNS]; /* the ink of each column, INK_COUNT where nothing was painted */

static void record(cairo_t *cr G_GNUC_UNUSED, int column, int length, Ink ink)
{
    for (int i = column; i < column + length && i < COLUMNS; i++)
        painted[i] = ink;
}

/* Lexes one line; a cairo context is needed only to say "paint". */
static gboolean lex(const char *line, gboolean comment)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surface);
    gboolean result;

    for (int i = 0; i < COLUMNS; i++)
        painted[i] = INK_COUNT;
    result = lex_line(line, (int)strlen(line), comment, cr, record);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return result;
}

/* Every column of the first occurrence of word in line has ink. */
static void assert_ink(const char *line, const char *word, Ink ink)
{
    const char *at = strstr(line, word);

    g_assert_nonnull(at);
    for (int i = (int)(at - line); i < (int)(at - line) + (int)strlen(word); i++)
        if (painted[i] != ink)
            g_error("\"%s\" in \"%s\": column %d has ink %d, not %d", word, line, i, painted[i], ink);
}

static void test_words(void)
{
    const char *line = "static int foo (void) { return bar(x) + y; }";

    lex(line, FALSE);
    assert_ink(line, "static", INK_KEYWORD);
    assert_ink(line, "int", INK_KEYWORD);
    assert_ink(line, "foo", INK_FUNCTION); /* blanks before "(" still make a call */
    assert_ink(line, "(", INK_PUNCTUATION);
    assert_ink(line, "void", INK_KEYWORD);
    assert_ink(line, "return", INK_STATEMENT);
    assert_ink(line, "bar", INK_FUNCTION);
    assert_ink(line, "x", INK_TEXT);
    assert_ink(line, "+", INK_PUNCTUATION);
    assert_ink(line, ";", INK_PUNCTUATION);
}

static void test_names(void)
{
    const char *line = "X y; GtkWidget *w = MAX(a, MIN_SIZE) + G_SOURCE_REMOVE; App *t;";

    lex(line, FALSE);
    assert_ink(line, "GtkWidget", INK_TYPE);
    assert_ink(line, "*", INK_PUNCTUATION);
    assert_ink(line, "MAX", INK_KEYWORD); /* a macro call */
    assert_ink(line, "MIN_SIZE", INK_CONSTANT);
    assert_ink(line, "G_SOURCE_REMOVE", INK_CONSTANT);
    assert_ink(line, "App", INK_TYPE);
    assert_ink(line, "X", INK_TEXT); /* one capital letter is not a constant */
}

static void test_types(void)
{
    const char *line = "bool b = TRUE; size_t n; gboolean ok; FILE *f; my_type_t m;";

    lex(line, FALSE);
    assert_ink(line, "size_t", INK_TYPE);
    assert_ink(line, "gboolean", INK_TYPE);
    assert_ink(line, "FILE", INK_TYPE);
    assert_ink(line, "my_type_t", INK_TYPE);
    assert_ink(line, "bool", INK_KEYWORD);
    assert_ink(line, "TRUE", INK_KEYWORD);
}

static void test_literals(void)
{
    const char *line = "x = \"a b\" + 'c' + 0x1F + 2.5; // note";

    lex(line, FALSE);
    assert_ink(line, "\"a b\"", INK_STRING);
    assert_ink(line, "'c'", INK_STRING);
    assert_ink(line, "0x1F", INK_NUMBER);
    assert_ink(line, "2.5", INK_NUMBER);
    assert_ink(line, "// note", INK_COMMENT);
}

static void test_comments(void)
{
    const char *open = "int a; /* starts";
    const char *close = "ends */ int b;";

    g_assert_true(lex(open, FALSE));
    assert_ink(open, "/* starts", INK_COMMENT);
    g_assert_false(lex(close, TRUE));
    assert_ink(close, "ends */", INK_COMMENT);
    assert_ink(close, "int", INK_KEYWORD);
}

static void test_preprocessor(void)
{
    const char *include = "#include <stdio.h>";
    const char *local = "  #  include \"app.h\"";
    const char *define = "#define LIMIT 10";
    const char *condition = "#if defined(X) && Y > 1 // why";
    const char *end = "#endif";

    lex(include, FALSE);
    assert_ink(include, "#include", INK_STATEMENT);
    assert_ink(include, "<stdio.h>", INK_STRING);
    lex(local, FALSE);
    assert_ink(local, "#  include", INK_STATEMENT);
    assert_ink(local, "\"app.h\"", INK_STRING);
    lex(define, FALSE);
    assert_ink(define, "#define", INK_STATEMENT);
    assert_ink(define, "LIMIT", INK_KEYWORD);
    assert_ink(define, "10", INK_NUMBER);
    lex(condition, FALSE);
    assert_ink(condition, "#if", INK_STATEMENT);
    assert_ink(condition, "defined(X) && Y > 1", INK_KEYWORD);
    assert_ink(condition, "// why", INK_COMMENT);
    lex(end, FALSE);
    assert_ink(end, "#endif", INK_STATEMENT);
}

/* Problems ------------------------------------------------------------------------------------- */

static App app;
static Tab tab;

static void test_problem_spec(void)
{
    minimap_set_problems(&tab, "e3,w7,e10,x4,w0,e");
    g_assert_cmpuint(tab.view->problem_lines->len, ==, 3);
    Problem *p = (Problem *)tab.view->problem_lines->data;
    g_assert_cmpint(p[0].line, ==, 2);
    g_assert_true(p[0].error);
    g_assert_cmpint(p[1].line, ==, 6);
    g_assert_false(p[1].error);
    g_assert_cmpint(p[2].line, ==, 9);
    g_assert_true(p[2].error);

    minimap_set_problems(&tab, "-");
    g_assert_cmpuint(tab.view->problem_lines->len, ==, 0);
}

/* The color in the middle of a pixel. */
static void pixel(cairo_surface_t *surface, int x, int y, int *r, int *g, int *b)
{
    guint32 value = *(guint32 *)(cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4);

    *r = (value >> 16) & 0xff;
    *g = (value >> 8) & 0xff;
    *b = value & 0xff;
}

static void test_problem_drawing(void)
{
    int width = MINIMAP_WIDTH + 4, height = 200, r, g, b;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cairo_t *cr = cairo_create(surface);

    tab.lines = 20;
    tab.height = 0; /* no viewport shade */
    minimap_set_problems(&tab, "e2,w4");
    draw_minimap(NULL, cr, width, height, tab.view);
    cairo_surface_flush(surface);

    /* Line 2 is red across the whole width, also right of the text and in the margin. */
    for (int x = 0; x < width; x++) {
        pixel(surface, x, 1 * LINE_PITCH + 1, &r, &g, &b);
        g_assert_cmpint(r, ==, 0xf1);
        g_assert_cmpint(g, ==, 0x4c);
        g_assert_cmpint(b, ==, 0x4c);
    }
    pixel(surface, width - 1, 3 * LINE_PITCH + 1, &r, &g, &b);
    g_assert_cmpint(r, ==, 0xcc);
    g_assert_cmpint(g, ==, 0xa7);
    g_assert_cmpint(b, ==, 0x00);
    pixel(surface, width - 1, 2 * LINE_PITCH + 1, &r, &g, &b); /* line 3: nothing */
    g_assert_cmpint(r + g + b, ==, 0);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

int main(int argc, char **argv)
{
    char *dir, *path;
    int status;

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/minimap/lexer/words", test_words);
    g_test_add_func("/minimap/lexer/names", test_names);
    g_test_add_func("/minimap/lexer/types", test_types);
    g_test_add_func("/minimap/lexer/literals", test_literals);
    g_test_add_func("/minimap/lexer/comments", test_comments);
    g_test_add_func("/minimap/lexer/preprocessor", test_preprocessor);
    if (!gtk_init_check()) {
        g_test_message("no display: problem tests skipped");
        return g_test_run();
    }

    /* An editor view on a small C file, without the terminal and the control pipe. */
    dir = g_dir_make_tmp("t-code-minimap-XXXXXX", NULL);
    path = g_build_filename(dir, "a.c", NULL);
    g_assert_true(g_file_set_contents(path, "int a;\n\n\n\n\n", -1, NULL));
    tab.app = &app;
    tab.path = path;
    tab.view = g_new0(EditorView, 1);
    tab.view->tab = &tab;
    tab.view->c_like = TRUE;
    tab.view->control_fd = -1;
    tab.view->minimap = gtk_drawing_area_new();
    tab.view->starts = g_array_new(FALSE, FALSE, sizeof(gsize));
    tab.view->in_comment = g_array_new(FALSE, FALSE, sizeof(gboolean));
    tab.view->problem_lines = g_array_new(FALSE, FALSE, sizeof(Problem));
    editor_page(VTE_TERMINAL(vte_terminal_new())); /* sets up the inks */
    load_text(tab.view);

    g_test_add_func("/minimap/problems/spec", test_problem_spec);
    g_test_add_func("/minimap/problems/drawing", test_problem_drawing);
    status = g_test_run();

    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
    return status;
}
