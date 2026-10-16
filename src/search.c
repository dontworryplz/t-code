/*
 * Ctrl+Shift+F: search the text of every file in the project, like VS Code's Search.
 * ripgrep does the work (it skips what .gitignore ignores and binary files); grep stands in when
 * it is missing. Picking a result opens the file at that line.
 */
#include "app.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    guint generation;
    char *text;
} Search;

static guint debounce_id;
static Search *pending; /* waiting for typing to pause */
static GCancellable *running;

static void search_free(Search *s)
{
    g_free(s->text);
    g_free(s);
}

/* rest is "line:column:text" (ripgrep) or "line:text" (grep). */
static void add_result(Search *s, const char *file, char *rest, gboolean has_column)
{
    char *fields[2] = { NULL };
    char *p = rest;

    for (int i = 0; i < (has_column ? 2 : 1); i++) {
        fields[i] = p;
        p = strchr(p, ':');
        if (!p)
            return;
        *p++ = '\0';
    }
    const char *path = g_str_has_prefix(file, "./") ? file + 2 : file;
    char *text = g_strstrip(g_strndup(p, 200));
    char *detail = g_strdup_printf("%s:%s", path, fields[0]);
    char *payload = g_strdup_printf("%s\n%s\n%s", path, fields[0], has_column ? fields[1] : "1");

    picker_add(s->app, s->generation, *text ? text : "(empty line)", detail, payload);
    g_free(payload);
    g_free(detail);
    g_free(text);
}

static void on_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Search *s = data;
    GBytes *out = NULL;
    GError *error = NULL;
    int count = 0;

    if (!g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            picker_message(s->app, s->generation, error->message);
        g_error_free(error);
        search_free(s);
        return;
    }
    gsize size;
    const char *bytes = g_bytes_get_data(out, &size);
    gboolean has_column = GPOINTER_TO_INT(g_object_get_data(source, "has-column"));

    /* Each line is "path\0rest": a ':' in a file name cannot split it. */
    for (const char *start = bytes, *end = bytes + size; start && start < end;) {
        const char *newline = memchr(start, '\n', end - start);
        gsize len = (newline ? newline : end) - start;
        const char *nul = memchr(start, '\0', len);
        if (nul) {
            char *path = g_utf8_make_valid(start, nul - start);
            char *rest = g_utf8_make_valid(nul + 1, start + len - nul - 1);
            add_result(s, path, rest, has_column);
            count++;
            g_free(rest);
            g_free(path);
        }
        start = newline ? newline + 1 : end;
    }
    if (count == 0)
        picker_message(s->app, s->generation, "No results");
    g_bytes_unref(out);
    search_free(s);
}

static gboolean start(gpointer data)
{
    Search *s = data;
    char *rg = g_find_program_in_path("rg");
    const char *rg_argv[] = { "rg", "--line-number", "--column", "--no-heading", "--color", "never",
                              "--smart-case", "--fixed-strings", "--max-columns", "300",
                              "--max-count", "100", "--null", "--", s->text, ".", NULL };
    const char *grep_argv[] = { "grep", "-rnIFZ", "--exclude-dir=.git", "--exclude-dir=build",
                                "--", s->text, ".", NULL };
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                              G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    GError *error = NULL;
    GSubprocess *proc;

    debounce_id = 0;
    pending = NULL;
    g_subprocess_launcher_set_cwd(launcher, s->app->root);
    proc = g_subprocess_launcher_spawnv(launcher, rg ? rg_argv : grep_argv, &error);
    g_object_unref(launcher);
    if (!proc) {
        picker_message(s->app, s->generation, error->message);
        g_error_free(error);
        search_free(s);
        g_free(rg);
        return G_SOURCE_REMOVE;
    }
    g_object_set_data(G_OBJECT(proc), "has-column", GINT_TO_POINTER(rg != NULL));
    running = g_cancellable_new();
    g_subprocess_communicate_async(proc, NULL, running, on_done, s);
    g_object_unref(proc);
    g_free(rg);
    return G_SOURCE_REMOVE;
}

static void query(App *t, const char *text, guint generation)
{
    if (debounce_id) {
        g_source_remove(debounce_id);
        search_free(pending);
        debounce_id = 0;
        pending = NULL;
    }
    if (running) {
        g_cancellable_cancel(running);
        g_clear_object(&running);
    }
    if (strlen(text) < 2) {
        picker_message(t, generation, "Type at least two characters to search all files");
        return;
    }

    Search *s = g_new0(Search, 1);
    s->app = t;
    s->generation = generation;
    s->text = g_strdup(text);
    picker_message(t, generation, "Searching…");
    pending = s;
    debounce_id = g_timeout_add(200, start, s); /* wait until typing pauses */
}

static void choose(App *t, const char *payload)
{
    char **parts = g_strsplit(payload, "\n", 3);
    char *path = g_build_filename(t->root, parts[0], NULL);

    open_file_at(t, path, atoi(parts[1]), parts[2] ? atoi(parts[2]) : 1);
    g_free(path);
    g_strfreev(parts);
}

void search_show(App *t)
{
    picker_open(t, "Search in files", query, choose);
}
