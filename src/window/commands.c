/*
 * Every command of the window in one table: the keyboard shortcuts, the command palette
 * (Ctrl+Shift+P) and the Keybinds list are all made from it.
 */
#include "window/commands.h"

#include "dialogs/about.h"
#include "dialogs/keybinds.h"
#include "dialogs/settings.h"
#include "editor/editing.h"
#include "editor/editor.h"
#include "editor/findbar.h"
#include "editor/preview.h"
#include "editor/tabs.h"
#include "lsp/client.h"
#include "lsp/completion.h"
#include "lsp/navigate.h"
#include "lsp/signature.h"
#include "git/compare.h"
#include "popups/picker.h"
#include "popups/quickopen.h"
#include "project/project.h"
#include "project/run.h"
#include "window/statusbar.h"
#include "window/window.h"

#include <stdlib.h>

typedef struct {
  const char *name;
  const char *trigger; /* GTK shortcut trigger, NULL when there is none */
  const char *keys;    /* the shortcut as shown */
  void (*run)(App *t);
  gboolean editor; /* the shortcut works in an editor (or its find bar) only: elsewhere the
                          * key goes on (Ctrl+W deletes a word in the shell) */
} Command;

/* What the commands do ----------------------------------------------------------------------- */

/* clang-format off */
static void build(App *t) { build_project(t, FALSE); }
static void run(App *t) { build_project(t, TRUE); }
static void next_tab(App *t) { step_tab(t, 1); }
static void prev_tab(App *t) { step_tab(t, -1); }
static void focus_left(App *t) { focus_group(t, 0); }
static void focus_right(App *t) { focus_group(t, 1); }
static void zoom_in(App *t) { set_font_scale(t, t->font_scale * 1.1); }
static void zoom_out(App *t) { set_font_scale(t, t->font_scale / 1.1); }
static void zoom_reset(App *t) { set_font_scale(t, 1.0); }
static void toggle_files(App *t) { toggle_pane(t, t->sidebar); }
static void toggle_shell(App *t) { toggle_pane(t, GTK_WIDGET(t->shell)); }
static void show_scm(App *t) { show_sidebar_page(t, "scm"); }
static void show_search(App *t) { show_sidebar_page(t, "search"); }
/* clang-format on */

/* Alt+1: the file manager, showing the sidebar. */
static void focus_fm(App *t)
{
  show_sidebar_page(t, "files");
}

/* Alt+3: the shell, showing it. */
static void focus_shell(App *t)
{
  set_pane_visible(t, GTK_WIDGET(t->shell), TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(t->shell));
}

/* Ctrl+\: split the editor area, or join it again. */
static void toggle_split(App *t)
{
  set_split(t, !gtk_widget_get_visible(GTK_WIDGET(t->groups[1])));
  focus_current_tab(t);
}

/* The current editor tab, or NULL. */
static Tab *current_editor(App *t)
{
  Tab *tab = current_tab(t);

  return tab && tab->kind == TAB_EDITOR ? tab : NULL;
}

/* Ctrl+S */
static void save(App *t)
{
  Tab *tab = current_editor(t);

  if (tab)
    save_with_format(tab);
}

/* Ctrl+W: closes the current tab (an editor asks about unsaved changes). */
static void close_current(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR)
    quit_editor(tab);
  else if (tab)
    remove_tab(tab);
}

/* The editing commands, on the current editor. */
/* clang-format off */
static void find(App *t) { Tab *tab = current_editor(t); if (tab) findbar_show(tab, FALSE); }
static void replace(App *t) { Tab *tab = current_editor(t); if (tab) findbar_show(tab, TRUE); }
static void find_next(App *t) { Tab *tab = current_editor(t); if (tab) findbar_next(tab, FALSE); }
static void find_previous(App *t) { Tab *tab = current_editor(t); if (tab) findbar_next(tab, TRUE); }
static void comment(App *t) { Tab *tab = current_editor(t); if (tab) editor_toggle_comment(tab); }
static void duplicate(App *t) { Tab *tab = current_editor(t); if (tab) editor_duplicate_lines(tab); }
static void line_up(App *t) { Tab *tab = current_editor(t); if (tab) editor_move_lines(tab, FALSE); }
static void line_down(App *t) { Tab *tab = current_editor(t); if (tab) editor_move_lines(tab, TRUE); }
static void delete_line(App *t) { Tab *tab = current_editor(t); if (tab) editor_delete_lines(tab); }
static void redo(App *t) { Tab *tab = current_editor(t); if (tab) editor_redo(tab); }
static void definition(App *t) { Tab *tab = current_editor(t); if (tab) go_to_definition(tab); }
static void references(App *t) { Tab *tab = current_editor(t); if (tab) find_references(tab); }
static void format(App *t) { Tab *tab = current_editor(t); if (tab) format_document(tab); }
static void rename_it(App *t) { Tab *tab = current_editor(t); if (tab) rename_symbol(tab); }
static void suggest(App *t) { Tab *tab = current_editor(t); if (tab) completion_show(tab); }
static void parameter_hints(App *t) { Tab *tab = current_editor(t); if (tab) signature_show(tab); }
/* clang-format on */

/* Go to Line: the picker offers the line typed so far. */
static void line_query(App *t, const char *text, guint generation)
{
  Tab *tab = current_editor(t);
  int lines = tab ? gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(tab->buffer)) : 0;
  char *end;
  long line = strtol(text, &end, 10);

  if (*text && *end == '\0' && line >= 1 && line <= lines) {
    char *label = g_strdup_printf("Go to line %ld", line), *payload = g_strdup_printf("%ld", line);
    picker_add(t, generation, label, NULL, payload);
    g_free(payload);
    g_free(label);
  } else {
    char *message = g_strdup_printf("Type a line number between 1 and %d", lines);
    picker_message(t, generation, message);
    g_free(message);
  }
}

static void line_choose(App *t, const char *line)
{
  Tab *tab = current_editor(t);

  if (tab) {
    select_tab(tab);
    editor_goto(tab, atoi(line), 1);
  }
}

/* Ctrl+G */
static void go_to_line(App *t)
{
  if (current_editor(t))
    picker_open(t, "Line number", line_query, line_choose);
}

/* Ctrl+Shift+V: a preview of the current Markdown file. */
static void preview(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR && is_markdown(tab->path))
    open_preview(t, tab->path);
}

/* Ctrl+Alt+D: the current file next to its last committed version. */
static void compare_head(App *t)
{
  const char *path = comparable_path(t);

  if (path)
    compare_with_head(t, path);
}

/* Ctrl+Alt+H: the current file's commits, to compare with one. */
static void history(App *t)
{
  const char *path = comparable_path(t);

  if (path)
    file_history(t, path);
}

/* The table ---------------------------------------------------------------------------------- */

static const Command commands[] = {
  { "Save", "<Control>s", "Ctrl+S", save, TRUE },
  { "Close Tab", "<Control>w|<Control>q", "Ctrl+W", close_current, TRUE },
  { "Go to File…", "<Control>p", "Ctrl+P", quick_open_show, FALSE },
  { "Search in Files", "<Control><Shift>f", "Ctrl+Shift+F", show_search, FALSE },
  { "Command Palette…", "<Control><Shift>p|F1", "Ctrl+Shift+P, F1", command_palette, FALSE },
  { "Go to Line…", "<Control>g", "Ctrl+G", go_to_line, TRUE },
  { "Find", "<Control>f", "Ctrl+F", find, TRUE },
  { "Replace", "<Control>h", "Ctrl+H", replace, TRUE },
  { "Find Next", "F3", "F3, Enter", find_next, TRUE },
  { "Find Previous", "<Shift>F3", "Shift+F3, Shift+Enter", find_previous, TRUE },
  { "Toggle Line Comment", "<Control>slash|<Control>KP_Divide|<Alt>slash", "Ctrl+/", comment, TRUE },
  { "Duplicate Line", "<Control>d|<Shift><Alt>Down", "Ctrl+D", duplicate, TRUE },
  { "Move Line Up", "<Alt>Up", "Alt+↑", line_up, TRUE },
  { "Move Line Down", "<Alt>Down", "Alt+↓", line_down, TRUE },
  { "Delete Line", "<Control><Shift>k", "Ctrl+Shift+K", delete_line, TRUE },
  { "Redo", "<Control>y", "Ctrl+Y, Ctrl+Shift+Z", redo, TRUE },
  { "Go to Definition", "F12|<Alt>d", "F12, Ctrl+Click", definition, TRUE },
  { "Go Back", "<Alt>Left", "Alt+←", go_back, TRUE },
  { "Find All References", "<Shift>F12|<Alt>r", "Shift+F12", references, TRUE },
  { "Rename Symbol", "F2", "F2", rename_it, TRUE },
  { "Format Document", "<Shift><Alt>f|<Alt>f", "Shift+Alt+F", format, TRUE },
  { "Trigger Suggest", "<Control>space", "Ctrl+Space", suggest, TRUE },
  { "Trigger Parameter Hints", "<Control><Shift>space", "Ctrl+Shift+Space", parameter_hints, TRUE },
  { "Build", "<Control><Shift>b", "Ctrl+Shift+B", build, FALSE },
  { "Build and Run", "F5", "F5", run, FALSE },
  { "Git: Compare with HEAD", "<Control><Alt>d", "Ctrl+Alt+D", compare_head, FALSE },
  { "Git: File History…", "<Control><Alt>h", "Ctrl+Alt+H", history, FALSE },
  { "Git: Stop Comparing", NULL, NULL, stop_comparing, FALSE },
  { "Markdown Preview", "<Control><Shift>v", "Ctrl+Shift+V", preview, FALSE },
  { "Split Editor", "<Control>backslash", "Ctrl+\\", toggle_split, FALSE },
  { "Focus Left Editor Group", "<Control>1", "Ctrl+1", focus_left, FALSE },
  { "Focus Right Editor Group", "<Control>2", "Ctrl+2", focus_right, FALSE },
  { "Next Tab", "<Control>Page_Down|<Alt>period", "Ctrl+PageDown, Alt+.", next_tab, FALSE },
  { "Previous Tab", "<Control>Page_Up|<Alt>comma", "Ctrl+PageUp, Alt+,", prev_tab, FALSE },
  { "Focus File Manager", "<Alt>1", "Alt+1", focus_fm, FALSE },
  { "Focus Editor", "<Alt>2", "Alt+2", focus_current_tab, FALSE },
  { "Focus Shell", "<Alt>3", "Alt+3", focus_shell, FALSE },
  { "Toggle Sidebar", "<Control>b", "Ctrl+B", toggle_files, FALSE },
  { "Toggle Shell", "<Control>j", "Ctrl+J", toggle_shell, FALSE },
  { "Source Control", "<Control><Shift>g", "Ctrl+Shift+G", show_scm, FALSE },
  { "Toggle Code Coloring", "F7", "F7", toggle_syntax, FALSE },
  { "clangd: Restart Language Server", NULL, NULL, lsp_restart, FALSE },
  { "Change Indentation for This Project…", NULL, NULL, status_pick_indent, FALSE },
  { "Zoom In", "<Control>equal|<Control>plus|<Control>KP_Add", "Ctrl+=", zoom_in, FALSE },
  { "Zoom Out", "<Control>minus|<Control>KP_Subtract", "Ctrl+-", zoom_out, FALSE },
  { "Reset Zoom", "<Control>0|<Control>KP_0", "Ctrl+0", zoom_reset, FALSE },
  { "Open Folder…", "<Control><Shift>o", "Ctrl+Shift+O", choose_folder, FALSE },
  { "Settings…", "<Control>comma", "Ctrl+,", show_settings, FALSE },
  { "Keyboard Shortcuts", NULL, NULL, show_keybinds, FALSE },
  { "About t-code", NULL, NULL, show_about, FALSE },
};

const KeyHelp *window_keys(int *n)
{
  static KeyHelp keys[G_N_ELEMENTS(commands)];

  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
    keys[i] = (KeyHelp){ commands[i].name, commands[i].keys };
  *n = G_N_ELEMENTS(commands);
  return keys;
}

/* Shortcuts ---------------------------------------------------------------------------------- */

/* Whether the keyboard is in an editor's page: its text or its find bar. */
static gboolean in_editor(GtkWidget *focus)
{
  for (; focus; focus = gtk_widget_get_parent(focus))
    if (gtk_widget_has_css_class(focus, "tcode-editor"))
      return TRUE;
  return FALSE;
}

static gboolean on_shortcut(GtkWidget *window, GVariant *args G_GNUC_UNUSED, gpointer data)
{
  const Command *command = data;

  if (command->editor && !in_editor(gtk_root_get_focus(GTK_ROOT(window))))
    return FALSE;
  command->run(g_object_get_data(G_OBJECT(window), "t-code-app"));
  return TRUE;
}

void add_shortcuts(App *t)
{
  GtkEventController *controller = gtk_shortcut_controller_new();

  g_object_set_data(G_OBJECT(t->window), "t-code-app", t);
  /* Capture phase: the window sees these keys before the focused terminal or editor does. */
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
    if (!commands[i].trigger)
      continue;
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(controller),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string(commands[i].trigger),
                         gtk_callback_action_new(on_shortcut, (gpointer)&commands[i], NULL)));
  }
  gtk_widget_add_controller(GTK_WIDGET(t->window), controller);
}

/* Command palette ---------------------------------------------------------------------------- */

typedef struct {
  int index, score;
} Match;

/* Best match first; equal ones in table order. */
static int by_score(gconstpointer a, gconstpointer b)
{
  const Match *x = a, *y = b;

  return x->score != y->score ? y->score - x->score : x->index - y->index;
}

/* The table order when nothing is typed, otherwise the best match first. */
static void query(App *t, const char *text, guint generation)
{
  Match matches[G_N_ELEMENTS(commands)];
  int n = 0;

  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
    int score = *text ? fuzzy_score(text, commands[i].name) : 0;
    if (score >= 0)
      matches[n++] = (Match){ (int)i, score };
  }
  qsort(matches, n, sizeof *matches, (int (*)(const void *, const void *))by_score);
  for (int i = 0; i < n; i++) {
    const Command *c = &commands[matches[i].index];
    char *index = g_strdup_printf("%d", matches[i].index);
    picker_add(t, generation, c->name, c->keys, index);
    g_free(index);
  }
  if (n == 0)
    picker_message(t, generation, "No matching commands");
}

/* PickerChoose: runs the picked command (its index in the table). */
static void choose(App *t, const char *index)
{
  commands[atoi(index)].run(t);
}

void command_palette(App *t)
{
  picker_open(t, "Type a command", query, choose);
}
