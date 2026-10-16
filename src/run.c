/*
 * Build (Ctrl+Shift+B) and run (F5), in the shell pane, also on the status bar buttons.
 * The project is found from the current file upwards:
 *   CMakeLists.txt  configure + build in build/; run starts the newest executable there
 *   Makefile        make; run uses "make run" when the Makefile has a run target
 *   neither         compile the current C/C++ file alone (with ASan/UBSan) into build/
 */
#include "app.h"

#include <string.h>

#define C_FLAGS "-std=c17 -Wall -Wextra -g -fsanitize=address,undefined"
#define CXX_FLAGS "-std=c++20 -Wall -Wextra -g -fsanitize=address,undefined"

typedef enum { PROJECT_NONE, PROJECT_CMAKE, PROJECT_MAKE, PROJECT_FILE } ProjectKind;

typedef struct {
    ProjectKind kind;
    char *root; /* the folder with CMakeLists.txt / Makefile, or the file itself */
} Project;

/* The nearest directory, from start upwards, that holds one of names. */
static char *find_up(const char *start, const char *const *names)
{
    char *dir = g_strdup(start);

    for (;;) {
        for (int i = 0; names[i]; i++) {
            char *path = g_build_filename(dir, names[i], NULL);
            gboolean found = g_file_test(path, G_FILE_TEST_IS_REGULAR);
            g_free(path);
            if (found)
                return dir;
        }
        char *parent = g_path_get_dirname(dir);
        gboolean top = strcmp(parent, dir) == 0;
        g_free(dir);
        dir = parent;
        if (top) {
            g_free(dir);
            return NULL;
        }
    }
}

static gboolean is_source(const char *file, gboolean *cxx)
{
    const char *ext = file ? strrchr(file, '.') : NULL;

    *cxx = ext && (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".cxx") == 0);
    return ext && (*cxx || strcmp(ext, ".c") == 0);
}

static Project find_project(App *t)
{
    static const char *const cmake[] = { "CMakeLists.txt", NULL };
    static const char *const make[] = { "Makefile", "makefile", "GNUmakefile", NULL };
    Tab *tab = current_tab(t);
    const char *file = tab && tab->kind == TAB_EDITOR ? tab->path : NULL;
    char *start = file ? g_path_get_dirname(file) : g_strdup(t->root);
    Project p = { PROJECT_NONE, NULL };
    gboolean cxx;

    if ((p.root = find_up(start, cmake)))
        p.kind = PROJECT_CMAKE;
    else if ((p.root = find_up(start, make)))
        p.kind = PROJECT_MAKE;
    else if (is_source(file, &cxx)) {
        p.kind = PROJECT_FILE;
        p.root = g_strdup(file);
    }
    g_free(start);
    return p;
}

static char *cmake_command(const char *root, gboolean run)
{
    char *build_dir = g_build_filename(root, "build", NULL);
    char *src = g_shell_quote(root), *build = g_shell_quote(build_dir);
    char *ninja = g_find_program_in_path("ninja");
    char *cmd = g_strdup_printf(
        "cmake -S %s -B %s %s-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null"
        " && cmake --build %s",
        src, build, ninja ? "-G Ninja " : "", build);

    if (run) {
        /* The newest executable anywhere under build/ (targets may sit in build/bin, build/src),
         * leaving out CMake's own checks and shared libraries. */
        char *with_run = g_strdup_printf(
            "%s && exe=$(find %s -type f -executable -not -path '*/CMakeFiles/*' -not -name '*.so*'"
            " -printf '%%T@ %%p\\n' | sort -nr | head -1 | cut -d' ' -f2-)"
            " && if [ -n \"$exe\" ]; then echo \"▶ $exe\" && \"$exe\"; else echo 'No executable in build/'; fi",
            cmd, build);
        g_free(cmd);
        cmd = with_run;
    }
    g_free(ninja);
    g_free(build);
    g_free(src);
    g_free(build_dir);
    return cmd;
}

static char *make_command(const char *root, gboolean run)
{
    char *dir = g_shell_quote(root);
    char *cmd = run ? g_strdup_printf(
                          "make --no-print-directory -C %s"
                          " && if make -C %s -n run >/dev/null 2>&1; then make -s --no-print-directory -C %s run;"
                          " else echo \"Build OK (no 'run' target in Makefile)\"; fi",
                          dir, dir, dir)
                    : g_strdup_printf("make --no-print-directory -C %s", dir);

    g_free(dir);
    return cmd;
}

static char *file_command(const char *file, gboolean run)
{
    gboolean cxx;
    is_source(file, &cxx);

    char *dir = g_path_get_dirname(file);
    char *base = g_path_get_basename(file);
    *strrchr(base, '.') = '\0';
    char *out_dir = g_build_filename(dir, "build", NULL);
    char *out_path = g_build_filename(out_dir, base, NULL);
    char *qdir = g_shell_quote(dir), *qfile = g_shell_quote(file);
    char *qout_dir = g_shell_quote(out_dir), *qout = g_shell_quote(out_path);
    char *cmd = g_strdup_printf("mkdir -p %s && %s %s %s -o %s -lm", qout_dir, cxx ? "g++" : "gcc",
                                cxx ? CXX_FLAGS : C_FLAGS, qfile, qout);

    if (run) {
        /* In a subshell: the shell stays in its own directory. */
        char *with_run = g_strdup_printf("%s && (cd %s && %s)", cmd, qdir, qout);
        g_free(cmd);
        cmd = with_run;
    }
    g_free(qout);
    g_free(qout_dir);
    g_free(qfile);
    g_free(qdir);
    g_free(out_path);
    g_free(out_dir);
    g_free(base);
    g_free(dir);
    return cmd;
}

char *project_summary(App *t)
{
    Project p = find_project(t);
    char *name = p.root ? g_path_get_basename(p.root) : NULL;
    char *text;

    switch (p.kind) {
    case PROJECT_CMAKE: text = g_strdup_printf("CMake: %s", name); break;
    case PROJECT_MAKE: text = g_strdup_printf("Make: %s", name); break;
    case PROJECT_FILE: text = g_strdup_printf("gcc: %s", name); break;
    default: text = g_strdup("Nothing to build"); break;
    }
    g_free(name);
    g_free(p.root);
    return text;
}

/* The commands use sh syntax (if/fi, var=$(...)); a shell that speaks something else (fish,
 * tcsh) gets them through sh -c. */
static char *for_user_shell(const char *command)
{
    static const char *const posix[] = { "sh", "bash", "zsh", "dash", "ksh", "mksh", "ash", "yash", NULL };
    const char *env = g_getenv("SHELL");
    char *path = env && *env ? g_strdup(env) : vte_get_user_shell(); /* as start_shell() picks it */
    char *shell = path ? g_path_get_basename(path) : NULL;
    gboolean ok = !shell || g_strv_contains(posix, shell);
    char *quoted = ok ? NULL : g_shell_quote(command);
    char *line = ok ? g_strdup(command) : g_strconcat("sh -c ", quoted, NULL);

    g_free(quoted);
    g_free(shell);
    g_free(path);
    return line;
}

static void run_saved(App *t, gpointer data)
{
    char *command = data;

    if (!t->closing) {
        char *line = for_user_shell(command);
        char *with_clear = g_strdup_printf("clear; %s", line);
        run_in_shell(t, with_clear);
        g_free(with_clear);
        g_free(line);
    }
    g_free(command);
}

void build_project(App *t, gboolean run)
{
    Project p = find_project(t);
    char *command;

    switch (p.kind) {
    case PROJECT_CMAKE: command = cmake_command(p.root, run); break;
    case PROJECT_MAKE: command = make_command(p.root, run); break;
    case PROJECT_FILE: command = file_command(p.root, run); break;
    default:
        command = g_strdup("echo 't-code: nothing to build here (open a C file, or add a Makefile or CMakeLists.txt)'");
        break;
    }
    /* Every changed file is saved first. */
    save_all(t, run_saved, command);
    g_free(p.root);
}
