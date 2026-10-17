/*
 * Finding t-code's helper programs and data next to its executable (see paths.h).
 *   build tree   build/t-code, build/t-code-fm, build/t-code-lsp, micro/
 *   installed    bin/t-code, libexec/t-code/t-code-fm (lib/t-code on Arch), share/t-code/micro
 */
#include "window/paths.h"

#include <string.h>
#include <unistd.h>

/* Looked up next to the executable: the installed layout (bin/t-code, libexec/t-code/t-code-fm,
 * share/t-code/micro) or the build tree (build/t-code, build/t-code-fm, micro). candidates are
 * paths relative to the executable's folder, tried in order; the first one that exists wins. */
static char *find_beside_exe(const char *const *candidates)
{
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname(exe) : g_get_current_dir();
    char *found = NULL;

    for (int i = 0; candidates[i] && !found; i++) {
        char *path = g_build_filename(dir, candidates[i], NULL);
        char *clean = g_canonicalize_filename(path, NULL);
        if (g_file_test(clean, G_FILE_TEST_EXISTS))
            found = g_strdup(clean);
        g_free(clean);
        g_free(path);
    }
    if (!found) /* spawning fails and says so inside the pane */
        found = g_build_filename(dir, candidates[0], NULL);
    g_free(dir);
    g_free(exe);
    return found;
}

/* A micro config the user cannot write to (/usr/share/t-code/micro, installed by a package):
 * micro keeps backups and history in its config dir and saves files through it, so editors get
 * ~/.local/state/t-code/micro instead, whose entries link to the shared files. A real file the
 * user put there in place of a link is left alone. Takes shared and returns the folder to use. */
static char *writable_micro_config(char *shared)
{
    char *own;
    GDir *dir;
    const char *name;

    if (access(shared, W_OK) == 0 || !g_file_test(shared, G_FILE_TEST_IS_DIR))
        return shared;
    own = g_build_filename(g_get_user_state_dir(), "t-code", "micro", NULL);
    if (g_mkdir_with_parents(own, 0700) != 0 || !(dir = g_dir_open(shared, 0, NULL))) {
        g_free(own);
        return shared;
    }
    while ((name = g_dir_read_name(dir))) {
        char *target = g_build_filename(shared, name, NULL);
        char *link = g_build_filename(own, name, NULL);
        char *current = g_file_read_link(link, NULL);
        GFile *file = g_file_new_for_path(link);
        GError *error = NULL;

        if (!current || strcmp(current, target) != 0) {
            if (current)
                g_file_delete(file, NULL, NULL);
            if (!g_file_test(link, G_FILE_TEST_EXISTS) &&
                !g_file_make_symbolic_link(file, target, NULL, &error)) {
                g_warning("Cannot link %s: %s", link, error->message);
                g_error_free(error);
            }
        }
        g_object_unref(file);
        g_free(current);
        g_free(link);
        g_free(target);
    }
    g_dir_close(dir);
    g_free(shared);
    return own;
}

void find_files(App *t)
{
    /* libexec/t-code (Fedora, Debian, ~/.local) or lib/t-code (Arch) */
    static const char *const fm[] = { "t-code-fm", "../libexec/t-code/t-code-fm",
                                      "../lib/t-code/t-code-fm", NULL };
    static const char *const lsp[] = { "t-code-lsp", "../libexec/t-code/t-code-lsp",
                                       "../lib/t-code/t-code-lsp", NULL };
    static const char *const micro[] = { "../share/t-code/micro", "../micro", NULL };
    /* Installed, the icon is in ~/.local/share/icons; from the build tree it is in desktop/. */
    static const char *const icons[] = { "../desktop", NULL };
    /* Old file versions for comparisons; the pid keeps two t-code windows apart. */
    char *versions = g_strdup_printf("t-code-%d-versions", getpid());

    t->fm_program = find_beside_exe(fm);
    t->lsp_program = find_beside_exe(lsp);
    t->micro_config = writable_micro_config(find_beside_exe(micro));
    t->icon_dir = find_beside_exe(icons);
    t->temp_dir = g_build_filename(g_get_user_runtime_dir(), versions, NULL);
    g_free(versions);
}
