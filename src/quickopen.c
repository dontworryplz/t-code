/*
 * Ctrl+P: open a file by typing part of its name, like VS Code's Go to File.
 * The file list comes from git when the project is a repository (so .gitignore applies),
 * otherwise from walking the project, skipping hidden folders and build output.
 */
#include "app.h"

#include <string.h>

#define MAX_WALK 20000

static GPtrArray *files; /* paths relative to the project root */

static gboolean from_git(App *t)
{
    char *argv[] = { "git", "-C", t->root, "ls-files", "-z", "-co", "--exclude-standard", NULL };
    char *out = NULL;
    int status;

    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      &out, NULL, &status, NULL) ||
        !g_spawn_check_wait_status(status, NULL)) {
        g_free(out);
        return FALSE;
    }
    for (const char *p = out; *p; p += strlen(p) + 1)
        g_ptr_array_add(files, g_strdup(p));
    g_free(out);
    return TRUE;
}

static void walk(const char *dir, const char *prefix)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;

    if (!d)
        return;
    while ((name = g_dir_read_name(d)) && files->len < MAX_WALK) {
        char *path = g_build_filename(dir, name, NULL);
        char *rel = *prefix ? g_strconcat(prefix, "/", name, NULL) : g_strdup(name);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            /* Linked folders are left out: a link to a parent ("ln -s .. loop") never ends. */
            if (name[0] != '.' && strcmp(name, "build") != 0 && strcmp(name, "node_modules") != 0 &&
                !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
                walk(path, rel);
            g_free(rel);
        } else {
            g_ptr_array_add(files, rel);
        }
        g_free(path);
    }
    g_dir_close(d);
}

typedef struct {
    const char *path;
    int score;
} Match;

static int by_score(gconstpointer a, gconstpointer b)
{
    const Match *x = a, *y = b;

    return x->score != y->score ? y->score - x->score : strcmp(x->path, y->path);
}

static void query(App *t, const char *text, guint generation)
{
    GArray *matches = g_array_new(FALSE, FALSE, sizeof(Match));

    for (guint i = 0; i < files->len; i++) {
        const char *path = g_ptr_array_index(files, i);
        int s = *text ? fuzzy_score(text, path) : 0;
        if (s >= 0) {
            Match m = { path, s };
            g_array_append_val(matches, m);
        }
    }
    if (*text)
        g_array_sort(matches, by_score);
    for (guint i = 0; i < matches->len; i++) {
        const char *path = g_array_index(matches, Match, i).path;
        char *name = g_path_get_basename(path);
        char *dir = g_path_get_dirname(path);
        picker_add(t, generation, name, strcmp(dir, ".") == 0 ? "" : dir, path);
        g_free(dir);
        g_free(name);
    }
    if (matches->len == 0)
        picker_message(t, generation, "No matching files");
    g_array_free(matches, TRUE);
}

static void choose(App *t, const char *relative)
{
    char *path = g_build_filename(t->root, relative, NULL);

    open_file(t, path);
    g_free(path);
}

void quick_open_show(App *t)
{
    if (!files)
        files = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(files, 0);
    if (!from_git(t))
        walk(t->root, "");
    picker_open(t, "Go to file by name", query, choose);
}
