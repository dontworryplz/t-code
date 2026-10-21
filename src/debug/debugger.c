/*
 * Debugging with gdb (see debugger.h). A session goes:
 *   F5          the project is built in the shell (project/run.c); the build ends with
 *               "t-code --send 'debug <program>'", and the control socket calls debug_launch()
 *   launch      gdb starts ("gdb -i dap", debug/dap.c) with the program's terminal as its
 *               inferior-tty; it answers "initialize", says "initialized", gets every breakpoint
 *               and "configurationDone", then starts the program
 *   stopped     a breakpoint, a step's end, a crash (ASan aborts, so its report stops at its line
 *               too): the stack's innermost frame with a file is shown in the editor
 *   F10 ...     next / stepIn / stepOut / continue, with the stopped thread
 *   terminated  the program ended: gdb is stopped too (one gdb per run)
 * Shift+F5 asks gdb to end the program ("disconnect"), and kills it when it does not within END_MS.
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700 /* ptsname() under -std=c23 */
#include "debug/debugger.h"

#include "debug/dap.h"
#include "editor/breakpoints.h"
#include "editor/editor.h"
#include "lsp/json.h"
#include "popups/picker.h"
#include "project/run.h"
#include "project/session.h"
#include "window/terminal.h"
#include "window/window.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define END_MS 2000     /* gdb gets this long to end the program before it is killed */
#define LATER_MS 150    /* the program's last output reaches its terminal before "exited" */
#define STACK_LEVELS 50 /* frames asked for to find one with a file */
#define PROGRAM_DEPTH 4 /* folders deep a Makefile's programs are looked for */
#define MAX_PROGRAMS 200

typedef enum {
  STARTING, /* gdb starts, the program is not running yet */
  RUNNING,
  STOPPED,
  ENDING, /* asked to end */
} State;

struct Debugger {
  App *app;
  Dap *dap;
  State state;
  gboolean configured; /* the breakpoints went before the start: each change goes on its own now */
  gint64 thread;       /* the thread that stopped last, which the steps go on with */
  char *reason;        /* why it stopped last ("breakpoint", "step", "signal") */
  gboolean restart;    /* build and start again once ended */
  guint end_id;        /* gdb is killed when it does not end the program in time */
};

/* The Debug panel, and what is between F5 and the start. One per window (one window per process). */
static struct {
  GtkWidget *status;
  GtkWidget *start, *pause, *over, *into, *out, *restart, *stop;
  char *tty;     /* the terminal's device: the program's stdin, stdout and stderr */
  char *cwd;     /* where the program being built for debugging will run; NULL: none is */
  char *pick_in; /* the folder the program picker lists */
  GPtrArray *programs;
} ui;

/* The terminal ------------------------------------------------------------------------------- */

/* A line of t-code's own in the program's terminal, dimmed (or red: an error). */
static void say(App *t, gboolean error, const char *text)
{
  long column = 0;
  char *line;

  if (!t->debug.term)
    return;
  vte_terminal_get_cursor_position(t->debug.term, &column, NULL);
  line = g_strdup_printf("%s%s%s\x1b[0m\r\n", column > 0 ? "\r\n" : "", error ? "\x1b[31m" : "\x1b[2m", text);
  vte_terminal_feed(t->debug.term, line, -1);
  g_free(line);
}

typedef struct {
  App *app;
  char *text;
} Line;

static gboolean say_now(gpointer data)
{
  Line *line = data;

  say(line->app, FALSE, line->text);
  g_free(line->text);
  g_free(line);
  return G_SOURCE_REMOVE;
}

/* The same a moment later: gdb tells of the program's end before its terminal has read the program's
 * last lines. */
static void say_later(App *t, char *text)
{
  Line *line = g_new(Line, 1);

  *line = (Line){ t, text };
  g_timeout_add(LATER_MS, say_now, line);
}

/* The panel's buttons and status follow the session. */
static void update_ui(App *t, const char *status)
{
  Debugger *s = t->debug.session;
  gboolean stopped = s && s->state == STOPPED;

  if (!ui.start)
    return;
  gtk_widget_set_sensitive(ui.start, !s || stopped);
  gtk_widget_set_tooltip_text(ui.start, stopped ? "Continue (F5)" : "Start Debugging (F5)");
  gtk_widget_set_sensitive(ui.pause, s && s->state == RUNNING);
  gtk_widget_set_sensitive(ui.over, stopped);
  gtk_widget_set_sensitive(ui.into, stopped);
  gtk_widget_set_sensitive(ui.out, stopped);
  gtk_widget_set_sensitive(ui.restart, s != NULL);
  gtk_widget_set_sensitive(ui.stop, s != NULL);
  if (status)
    gtk_label_set_text(GTK_LABEL(ui.status), status);
}

/* The session's end ------------------------------------------------------------------------- */

/* gdb and the program are gone (or are killed now): everything is let go. */
static void finish(Debugger *s)
{
  App *t = s->app;
  gboolean again = s->restart;

  t->debug.session = NULL;
  if (s->end_id)
    g_source_remove(s->end_id);
  dap_free(s->dap);
  g_free(s->reason);
  g_free(s);
  if (t->closing) /* the tabs are gone */
    g_clear_pointer(&t->debug.stop_path, g_free);
  else
    breakpoints_show_stop(t, NULL, 0);
  update_ui(t, "Not debugging");
  if (again && !t->closing)
    debug_start_or_continue(t);
}

/* gdb did not end the program in time. */
static gboolean on_end_timeout(gpointer data)
{
  Debugger *s = data;

  s->end_id = 0;
  finish(s);
  return G_SOURCE_REMOVE;
}

static void on_disconnected(gpointer data, JsonNode *body G_GNUC_UNUSED, const char *error G_GNUC_UNUSED)
{
  finish(data);
}

/* Asks gdb to end the program; it is killed with gdb when that takes too long. */
static void end_session(Debugger *s)
{
  if (s->state == ENDING) { /* asked twice: no waiting */
    finish(s);
    return;
  }
  s->state = ENDING;
  breakpoints_show_stop(s->app, NULL, 0);
  update_ui(s->app, "Stopping…");
  dap_request(s->dap, "disconnect", "{\"terminateDebuggee\":true}", on_disconnected, s);
  s->end_id = g_timeout_add(END_MS, on_end_timeout, s);
}

void debug_end(App *t)
{
  if (t->debug.session) {
    t->debug.session->restart = FALSE;
    finish(t->debug.session);
  }
  g_clear_pointer(&ui.cwd, g_free);
}

/* Breakpoints -------------------------------------------------------------------------------- */

/* Tells gdb the breakpoints in path (none: it takes its own away). */
static void send_breakpoints(Debugger *s, const char *path)
{
  GArray *lines = breakpoints_in(s->app, path);
  GString *args = g_string_new("{\"source\":{\"path\":");
  char *quoted = json_quote(path);

  g_string_append_printf(args, "%s},\"breakpoints\":[", quoted);
  for (guint i = 0; lines && i < lines->len; i++)
    g_string_append_printf(args, "%s{\"line\":%d}", i ? "," : "", g_array_index(lines, int, i));
  g_string_append(args, "]}");
  dap_request(s->dap, "setBreakpoints", args->str, NULL, NULL);
  g_string_free(args, TRUE);
  g_free(quoted);
}

void debug_breakpoints_changed(App *t, const char *path)
{
  Debugger *s = t->debug.session;

  if (s && s->configured && s->state != ENDING)
    send_breakpoints(s, path);
}

/* Stopping ----------------------------------------------------------------------------------- */

/* Why the program stopped, for the status: "Paused on breakpoint", ... */
static const char *pause_reason(const char *reason)
{
  if (g_strcmp0(reason, "breakpoint") == 0)
    return "Paused on breakpoint";
  if (g_strcmp0(reason, "step") == 0)
    return "Paused after a step";
  if (g_strcmp0(reason, "pause") == 0)
    return "Paused";
  if (g_strcmp0(reason, "signal") == 0 || g_strcmp0(reason, "exception") == 0)
    return "Paused on a crash";
  return "Paused";
}

/* The stack of the thread that stopped: its innermost frame with a file on disk (not the C
 * library's, or ASan's) is shown. */
static void on_stack(gpointer data, JsonNode *body, const char *error)
{
  Debugger *s = data;
  App *t = s->app;
  JsonNode *frames = json_at(body, "stackFrames");
  guint n = frames && JSON_NODE_HOLDS_ARRAY(frames) ? json_array_get_length(json_node_get_array(frames)) : 0;
  const char *path = NULL, *top = NULL;
  int line = 0;

  if (s->state != STOPPED) /* went on meanwhile */
    return;
  for (guint i = 0; i < n && !path; i++) {
    JsonNode *frame = json_array_get_element(json_node_get_array(frames), i);
    const char *file = json_string_at(frame, "source.path");
    if (i == 0)
      top = json_string_at(frame, "name");
    if (file && g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
      path = file;
      line = (int)json_int_at(frame, "line", 0);
    }
  }
  if (path && line > 0) {
    char *name = g_path_get_basename(path);
    char *status = g_strdup_printf("%s: %s:%d", pause_reason(s->reason), name, line);
    breakpoints_show_stop(t, path, line);
    open_file_at(t, path, line, 1);
    update_ui(t, status);
    g_free(status);
    g_free(name);
  } else {
    char *where = g_strdup_printf("%s in %s (no source)", pause_reason(s->reason), top ? top : "?");
    if (error)
      say(t, TRUE, error);
    say(t, FALSE, where);
    update_ui(t, where);
    g_free(where);
  }
}

static void stopped(Debugger *s, JsonNode *body)
{
  const char *reason = json_string_at(body, "reason");
  char *args;

  s->state = STOPPED;
  s->thread = json_int_at(body, "threadId", s->thread);
  update_ui(s->app, pause_reason(reason));
  g_free(s->reason);
  s->reason = g_strdup(reason);
  args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT ",\"levels\":%d}", s->thread, STACK_LEVELS);
  dap_request(s->dap, "stackTrace", args, on_stack, s);
  g_free(args);
}

/* Events ------------------------------------------------------------------------------------- */

static void on_event(gpointer owner, const char *event, JsonNode *body)
{
  Debugger *s = owner;
  App *t = s->app;

  if (!event) { /* gdb went */
    if (s->state != ENDING)
      say(t, TRUE, "gdb ended unexpectedly");
    finish(s);
  } else if (strcmp(event, "initialized") == 0) {
    GHashTableIter iter;
    gpointer path;
    if (t->debug.breakpoints) {
      g_hash_table_iter_init(&iter, t->debug.breakpoints);
      while (g_hash_table_iter_next(&iter, &path, NULL))
        send_breakpoints(s, path);
    }
    s->configured = TRUE;
    dap_request(s->dap, "configurationDone", NULL, NULL, NULL);
  } else if (strcmp(event, "stopped") == 0) {
    stopped(s, body);
  } else if (strcmp(event, "continued") == 0 && s->state != ENDING) {
    s->state = RUNNING;
    breakpoints_show_stop(t, NULL, 0);
    update_ui(t, "Running");
  } else if (strcmp(event, "process") == 0 && s->state == STARTING) {
    s->state = RUNNING;
    update_ui(t, "Running");
  } else if (strcmp(event, "exited") == 0) {
    say_later(t, g_strdup_printf("The program exited with code %d", (int)json_int_at(body, "exitCode", 0)));
  } else if (strcmp(event, "terminated") == 0) {
    finish(s);
  } else if (strcmp(event, "output") == 0) {
    const char *category = json_string_at(body, "category"), *output = json_string_at(body, "output");
    if (output && (g_strcmp0(category, "important") == 0 || g_strcmp0(category, "stderr") == 0)) {
      char *text = g_strchomp(g_strdup(output));
      say(t, FALSE, text);
      g_free(text);
    }
  }
}

/* Starting ----------------------------------------------------------------------------------- */

/* ASan aborts at its first report (gdb stops there: the bad line shows), and its leak check is off:
 * it cannot run under a debugger. What the user set in ASAN_OPTIONS stays. */
static char **sanitizer_env(char **env)
{
  const char *now = g_environ_getenv(env, "ASAN_OPTIONS");
  GString *value = g_string_new(now ? now : "");

  if (!strstr(value->str, "detect_leaks"))
    g_string_append_printf(value, "%sdetect_leaks=0", value->len ? ":" : "");
  if (!strstr(value->str, "abort_on_error"))
    g_string_append_printf(value, "%sabort_on_error=1", value->len ? ":" : "");
  env = g_environ_setenv(env, "ASAN_OPTIONS", value->str, TRUE);
  g_string_free(value, TRUE);
  return env;
}

static void on_launched(gpointer data, JsonNode *body G_GNUC_UNUSED, const char *error)
{
  Debugger *s = data;

  if (error) {
    char *line = g_strdup_printf("Could not start the program: %s", error);
    say(s->app, TRUE, line);
    g_free(line);
    end_session(s);
  }
}

/* Starts gdb on program, to run in cwd. */
static void start(App *t, const char *program, const char *cwd)
{
  char *tty = g_strdup_printf("set inferior-tty %s", ui.tty ? ui.tty : "");
  const char *argv[] = { "gdb", "-q", "-iex", "set debuginfod enabled off", "-iex", tty, "-i", "dap", NULL };
  char **env = sanitizer_env(g_get_environ());
  GError *error = NULL;
  Debugger *s = g_new0(Debugger, 1);
  char *line, *args, *qprogram = json_quote(program), *qcwd = json_quote(cwd);

  s->app = t;
  s->state = STARTING;
  s->thread = 1;
  s->dap = dap_start(argv, cwd, env, on_event, s, &error);
  show_sidebar_page(t, "debug");
  if (!s->dap) {
    line = g_strdup_printf("Could not start gdb: %s", error->message);
    say(t, TRUE, line);
    g_free(line);
    g_free(s);
    update_ui(t, "Not debugging");
  } else {
    t->debug.session = s;
    line = g_strdup_printf("▶ %s", program);
    say(t, FALSE, line);
    g_free(line);
    args = g_strdup_printf("{\"program\":%s,\"cwd\":%s}", qprogram, qcwd);
    dap_request(s->dap, "launch", args, on_launched, s); /* answered once configurationDone went */
    g_free(args);
    update_ui(t, "Starting…");
  }
  g_clear_error(&error);
  g_strfreev(env);
  g_free(qcwd);
  g_free(qprogram);
  g_free(tty);
}

/* Which program (Makefile projects) ---------------------------------------------------------- */

/* Whether path is a program: an executable ELF file. */
static gboolean is_program(const char *path)
{
  char magic[4] = { 0 };
  FILE *file;
  gboolean elf;

  if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) || !g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
    return FALSE;
  if (!(file = fopen(path, "rb")))
    return FALSE;
  elf = fread(magic, 1, 4, file) == 4 && memcmp(magic,
                                                "\x7f"
                                                "ELF",
                                                4) == 0;
  fclose(file);
  return elf && !strstr(path, ".so");
}

/* The programs under dir, depth folders deep, into list. */
static void find_programs(const char *dir, int depth, GPtrArray *list)
{
  GDir *folder = g_dir_open(dir, 0, NULL);
  const char *name;

  while (folder && list->len < MAX_PROGRAMS && (name = g_dir_read_name(folder))) {
    char *path = g_build_filename(dir, name, NULL);
    if (name[0] == '.' || strcmp(name, "CMakeFiles") == 0 || strcmp(name, "node_modules") == 0)
      ;
    else if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
      if (depth > 0)
        find_programs(path, depth - 1, list);
    } else if (is_program(path)) {
      g_ptr_array_add(list, path);
      path = NULL;
    }
    g_free(path);
  }
  if (folder)
    g_dir_close(folder);
}

static gint64 modified(const char *path)
{
  GStatBuf st;

  return g_stat(path, &st) == 0 ? (gint64)st.st_mtime : 0;
}

/* Newest first. */
static int by_age(gconstpointer a, gconstpointer b)
{
  gint64 x = modified(*(char *const *)a), y = modified(*(char *const *)b);

  return x < y ? 1 : x > y ? -1 : 0;
}

static void program_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; ui.programs && i < ui.programs->len; i++) {
    const char *path = g_ptr_array_index(ui.programs, i);
    const char *relative = g_str_has_prefix(path, ui.pick_in) ? path + strlen(ui.pick_in) + 1 : path;
    if (*text && fuzzy_score(text, relative) < 0)
      continue;
    picker_add(t, generation, relative, NULL, path);
    shown++;
  }
  if (!shown) {
    char *message = ui.programs && ui.programs->len
                        ? g_strdup("No matching programs")
                        : g_strdup_printf("No programs in %s: build it first (Ctrl+Shift+B)", ui.pick_in);
    picker_message(t, generation, message);
    g_free(message);
  }
}

static void program_choose(App *t, const char *path)
{
  session_set_project_value(t, "debug-program", path);
  if (ui.cwd && !t->debug.session) { /* a build waits for it */
    char *cwd = g_steal_pointer(&ui.cwd);
    start(t, path, cwd);
    g_free(cwd);
  }
}

/* Lists the programs in dir in the picker. */
static void pick_program(App *t, const char *dir)
{
  g_free(ui.pick_in);
  ui.pick_in = g_strdup(dir);
  if (ui.programs)
    g_ptr_array_free(ui.programs, TRUE);
  ui.programs = g_ptr_array_new_with_free_func(g_free);
  find_programs(dir, PROGRAM_DEPTH, ui.programs);
  g_ptr_array_sort(ui.programs, by_age);
  picker_open(t, "Which program does F5 debug?", program_query, program_choose);
}

void debug_select_program(App *t)
{
  pick_program(t, t->project.root);
}

/* The program remembered for this project, if it is still there (free it). */
static char *remembered_program(App *t)
{
  char *path = session_project_value(t, "debug-program");

  if (path && !is_program(path))
    g_clear_pointer(&path, g_free);
  return path;
}

void debug_launch(App *t, const char *path)
{
  char *cwd, *program;

  if (t->debug.session) /* one at a time */
    return;
  if (!ui.cwd) /* not built by F5: run where it lies */
    ui.cwd = g_file_test(path, G_FILE_TEST_IS_DIR) ? g_strdup(path) : g_path_get_dirname(path);
  if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
    program = g_strdup(path);
  } else if (!(program = remembered_program(t))) {
    pick_program(t, path); /* starts once picked */
    return;
  }
  cwd = g_steal_pointer(&ui.cwd);
  start(t, program, cwd);
  g_free(cwd);
  g_free(program);
}

/* The commands ------------------------------------------------------------------------------- */

gboolean debug_active(App *t)
{
  return t->debug.session != NULL;
}

/* A request that sets the stopped program going (a step, continue). */
static void go(App *t, const char *command)
{
  Debugger *s = t->debug.session;
  char *args;

  if (!s || s->state != STOPPED)
    return;
  s->state = RUNNING;
  breakpoints_show_stop(t, NULL, 0);
  update_ui(t, "Running");
  args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT "}", s->thread);
  dap_request(s->dap, command, args, NULL, NULL);
  g_free(args);
}

void debug_start_or_continue(App *t)
{
  char *gdb;

  if (t->debug.session) {
    go(t, "continue");
    return;
  }
  if (!(gdb = g_find_program_in_path("gdb"))) {
    show_sidebar_page(t, "debug");
    say(t, TRUE, "gdb is not installed: install the package \"gdb\" (dnf, apt, pacman, xbps-install) to debug.");
    return;
  }
  g_free(gdb);
  g_free(ui.cwd);
  ui.cwd = build_for_debug(t);
  update_ui(t, "Building… (Terminal)");
}

/* clang-format off */
void debug_step_over(App *t) { go(t, "next"); }
void debug_step_into(App *t) { go(t, "stepIn"); }
void debug_step_out(App *t) { go(t, "stepOut"); }
/* clang-format on */

void debug_pause(App *t)
{
  Debugger *s = t->debug.session;
  char *args;

  if (!s || s->state != RUNNING)
    return;
  args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT "}", s->thread);
  dap_request(s->dap, "pause", args, NULL, NULL);
  g_free(args);
}

void debug_stop(App *t)
{
  if (t->debug.session)
    end_session(t->debug.session);
}

void debug_restart(App *t)
{
  if (t->debug.session) {
    t->debug.session->restart = TRUE;
    end_session(t->debug.session);
  } else {
    debug_start_or_continue(t);
  }
}

/* The panel ---------------------------------------------------------------------------------- */

static GtkWidget *tool_button(App *t, GtkWidget *bar, const char *icon, const char *tip, void (*run)(App *))
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);

  gtk_widget_set_tooltip_text(button, tip);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_focus_on_click(button, FALSE);
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(run), t);
  gtk_box_append(GTK_BOX(bar), button);
  return button;
}

/* The window is going: nothing touches the panel any more. */
static void on_panel_destroyed(GtkWidget *box G_GNUC_UNUSED, App *t)
{
  ui.status = ui.start = ui.pause = ui.over = ui.into = ui.out = ui.restart = ui.stop = NULL;
  t->debug.panel = NULL;
  t->debug.term = NULL;
}

/* The program's terminal: a pty of its own, whose other end gdb gives the programs. That end is
 * held open here, so the terminal stays between programs. */
static void open_pty(App *t)
{
  GError *error = NULL;
  VtePty *pty = vte_pty_new_sync(VTE_PTY_DEFAULT, NULL, &error);
  const char *name = pty ? ptsname(vte_pty_get_fd(pty)) : NULL;

  if (!name) {
    g_warning("t-code: no terminal for the debugger: %s", error ? error->message : "no pty name");
    g_clear_error(&error);
    g_clear_object(&pty);
    return;
  }
  ui.tty = g_strdup(name);
  if (open(ui.tty, O_RDWR | O_NOCTTY | O_CLOEXEC) < 0) /* held till t-code ends */
    g_warning("t-code: cannot open %s", ui.tty);
  vte_terminal_set_pty(t->debug.term, pty);
  g_object_unref(pty);
}

GtkWidget *debug_panel(App *t)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_widget_add_css_class(bar, "tcode-debug-bar");
  ui.start = tool_button(t, bar, "media-playback-start-symbolic", "Start Debugging (F5)", debug_start_or_continue);
  ui.pause = tool_button(t, bar, "media-playback-pause-symbolic", "Pause (F6)", debug_pause);
  ui.over = tool_button(t, bar, "go-jump-symbolic", "Step Over: the next line (F10)", debug_step_over);
  ui.into = tool_button(t, bar, "go-down-symbolic", "Step Into: into the function called here (F11)", debug_step_into);
  ui.out = tool_button(t, bar, "go-up-symbolic", "Step Out: back to the caller (Shift+F11)", debug_step_out);
  ui.restart = tool_button(t, bar, "view-refresh-symbolic", "Restart (Ctrl+Shift+F5)", debug_restart);
  ui.stop = tool_button(t, bar, "media-playback-stop-symbolic", "Stop (Shift+F5)", debug_stop);
  ui.status = gtk_label_new("Not debugging");
  gtk_label_set_xalign(GTK_LABEL(ui.status), 0);
  gtk_label_set_ellipsize(GTK_LABEL(ui.status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(ui.status, TRUE);
  gtk_widget_set_margin_start(ui.status, 8);
  gtk_widget_add_css_class(ui.status, "dim-label");
  gtk_box_append(GTK_BOX(bar), ui.status);
  gtk_box_append(GTK_BOX(box), bar);

  t->debug.term = new_terminal(t);
  gtk_widget_add_css_class(GTK_WIDGET(t->debug.term), "tcode-shell");
  vte_terminal_set_clear_background(t->debug.term, FALSE);
  open_pty(t);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(t->debug.term));
  say(t, FALSE,
      "F5 builds the project and runs it here under the debugger; click left of a line number (or F9) "
      "to stop at that line.");
  t->debug.panel = box;
  g_signal_connect(box, "destroy", G_CALLBACK(on_panel_destroyed), t);
  update_ui(t, NULL);
  return box;
}
