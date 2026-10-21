/*
 * Build (Ctrl+Shift+B), run (Ctrl+F5) and build for debugging (F5), in the shell pane, also on the
 * status bar buttons. The project is found from the current file upwards:
 *   CMakeLists.txt  configure + build in build/; run starts the project's executable there
 *                  (named after the project, else the newest one that is not a test)
 *   Makefile        make; run uses "make run" when the Makefile has a run target
 *   neither         compile the current C/C++ file alone (with ASan/UBSan) into build/
 * A build for debugging ends with "t-code --send 'debug <program>'" (or "debug-failed <dir>" when it
 * failed): the control socket hands the program to the debugger (for a Makefile, which builds who
 * knows what, the folder: the debugger asks which program, once).
 */
#include "project/run.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "window/terminal.h"

#include <string.h>

#define C_FLAGS "-std=c17 -Wall -Wextra -g -fsanitize=address,undefined"
#define CXX_FLAGS "-std=c++20 -Wall -Wextra -g -fsanitize=address,undefined"

typedef enum { PROJECT_NONE, PROJECT_CMAKE, PROJECT_MAKE, PROJECT_FILE } ProjectKind;

typedef enum {
  GOAL_BUILD,
  GOAL_RUN,
  GOAL_DEBUG, /* build, then hand the program to the debugger */
} Goal;

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

/* Whether file is C or C++ source (*cxx: C++). */
static gboolean is_source(const char *file, gboolean *cxx)
{
  const char *ext = file ? strrchr(file, '.') : NULL;

  *cxx = ext && (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".cxx") == 0);
  return ext && (*cxx || strcmp(ext, ".c") == 0);
}

/* What to build: CMake or Make from the current file's folder upwards, or else the current file
 * alone. */
static Project find_project(App *t)
{
  static const char *const cmake[] = { "CMakeLists.txt", NULL };
  static const char *const make[] = { "Makefile", "makefile", "GNUmakefile", NULL };
  Tab *tab = current_tab(t);
  const char *file = tab && tab->kind == TAB_EDITOR ? tab->path : NULL;
  char *start = file ? g_path_get_dirname(file) : g_strdup(t->project.root);
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

/* The shell command that sends "<verb> <what>" (what already a shell word: '/a/b', "$exe") to this
 * t-code through the control socket. */
static char *send_verb(const char *verb, const char *what)
{
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  char *quoted = g_shell_quote(exe ? exe : "t-code");
  char *command = g_strdup_printf("%s --send \"%s \"%s", quoted, verb, what);

  g_free(quoted);
  g_free(exe);
  return command;
}

/* The shell command that configures and builds with CMake (and then runs or debugs the program). */
static char *cmake_command(const char *root, Goal goal)
{
  char *build_dir = g_build_filename(root, "build", NULL);
  char *src = g_shell_quote(root), *build = g_shell_quote(build_dir);
  char *cache = g_build_filename(build_dir, "CMakeCache.txt", NULL);
  /* Ninja only for a new build/: CMake refuses another generator than the one it was configured
     * with (e.g. by hand with Unix Makefiles). */
  char *ninja = g_file_test(cache, G_FILE_TEST_EXISTS) ? NULL : g_find_program_in_path("ninja");
  char *cmd =
      g_strdup_printf("cmake -S %s -B %s %s-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null"
                      " && cmake --build %s",
                      src, build, ninja ? "-G Ninja " : "", build);

  if (goal != GOAL_BUILD) {
    char *debug = goal == GOAL_DEBUG ? send_verb("debug", "\"$exe\"") : NULL;
    /* The executable named after the project (CMAKE_PROJECT_NAME) anywhere under build/
         * (targets may sit in build/bin, build/src); else the newest one that is not a test, else
         * the newest one. CMake's own checks and shared libraries are left out. */
    char *with_run = g_strdup_printf("%s && exe=$(cd %s && {"
                                     " find . -type f -executable -not -path '*/CMakeFiles/*'"
                                     " -name \"$(sed -n 's/^CMAKE_PROJECT_NAME:STATIC=//p' CMakeCache.txt)\";"
                                     " find . -type f -executable -not -path '*/CMakeFiles/*' -not -name '*.so*'"
                                     " -printf '%%T@ %%p\\n' | sort -nr | cut -d' ' -f2- | grep -vi test;"
                                     " find . -type f -executable -not -path '*/CMakeFiles/*' -not -name '*.so*'"
                                     " -printf '%%T@ %%p\\n' | sort -nr | cut -d' ' -f2-; } | head -1)"
                                     " && if [ -n \"$exe\" ]; then exe=%s/${exe#./} && %s;"
                                     " else echo 'No executable in build/'; fi",
                                     cmd, build, build, debug ? debug : "echo \"▶ $exe\" && \"$exe\"");
    g_free(debug);
    g_free(cmd);
    cmd = with_run;
  }
  g_free(ninja);
  g_free(cache);
  g_free(build);
  g_free(src);
  g_free(build_dir);
  return cmd;
}

/* The shell command that runs make (and then "make run" when the Makefile has it, or the debugger). */
static char *make_command(const char *root, Goal goal)
{
  char *dir = g_shell_quote(root);
  char *debug = goal == GOAL_DEBUG ? send_verb("debug", dir) : NULL;
  char *cmd =
      debug ? g_strdup_printf("make --no-print-directory -C %s && %s", dir, debug)
      : goal == GOAL_RUN
          ? g_strdup_printf("make --no-print-directory -C %s"
                            " && if make -C %s -n run >/dev/null 2>&1; then make -s --no-print-directory -C %s run;"
                            " else echo \"Build OK (no 'run' target in Makefile)\"; fi",
                            dir, dir, dir)
          : g_strdup_printf("make --no-print-directory -C %s", dir);

  g_free(debug);
  g_free(dir);
  return cmd;
}

/* The shell command that compiles one file into build/ next to it (and then runs or debugs it). */
static char *file_command(const char *file, Goal goal)
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

  if (goal == GOAL_RUN) {
    /* In a subshell: the shell stays in its own directory. */
    char *with_run = g_strdup_printf("%s && (cd %s && %s)", cmd, qdir, qout);
    g_free(cmd);
    cmd = with_run;
  } else if (goal == GOAL_DEBUG) {
    char *debug = send_verb("debug", qout);
    char *with_debug = g_strdup_printf("%s && %s", cmd, debug);
    g_free(cmd);
    g_free(debug);
    cmd = with_debug;
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

/* The editors are saved: the command runs in the shell, after clearing it. */
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

/* The command for goal; *cwd (may be NULL) gets the folder the program runs in. */
static char *goal_command(App *t, Goal goal, char **cwd)
{
  Project p = find_project(t);
  char *command;

  switch (p.kind) {
    case PROJECT_CMAKE: command = cmake_command(p.root, goal); break;
    case PROJECT_MAKE: command = make_command(p.root, goal); break;
    case PROJECT_FILE: command = file_command(p.root, goal); break;
    default:
      command = g_strdup("echo 't-code: nothing to build here (open a C file, or add a Makefile or CMakeLists.txt)'");
      break;
  }
  if (cwd) /* the file's folder, as run does; the project's otherwise */
    *cwd = p.kind == PROJECT_FILE ? g_path_get_dirname(p.root) : g_strdup(p.root ? p.root : t->project.root);
  if (goal == GOAL_DEBUG && p.kind != PROJECT_NONE) { /* a failed build says so: the debugger stops waiting */
    char *dir = p.kind == PROJECT_FILE ? g_path_get_dirname(p.root) : g_strdup(p.root);
    char *qdir = g_shell_quote(dir), *failed = send_verb("debug-failed", qdir);
    char *with_failed = g_strdup_printf("%s || %s", command, failed);
    g_free(command);
    command = with_failed;
    g_free(failed);
    g_free(qdir);
    g_free(dir);
  }
  g_free(p.root);
  return command;
}

void build_project(App *t, gboolean run)
{
  /* Every changed file is saved first. */
  save_all(t, run_saved, goal_command(t, run ? GOAL_RUN : GOAL_BUILD, NULL));
}

char *build_for_debug(App *t)
{
  char *cwd = NULL;

  save_all(t, run_saved, goal_command(t, GOAL_DEBUG, &cwd));
  return cwd;
}
