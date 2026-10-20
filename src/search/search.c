/*
 * The Search panel, a tab next to the file manager and Source Control (Ctrl+Shift+F), like VS
 * Code's: the text to find with Match Case, Match Whole Word and Use Regular Expression, files to
 * include and exclude under "⋯", and the results grouped by file. Clicking a file's heading folds
 * its lines; clicking a line opens the file there.
 * ripgrep does the work (it skips what .gitignore ignores and binary files); grep stands in when
 * it is missing. Its output is read line by line as it comes, and searchmatch.c finds the matches
 * in each line to mark them.
 */
#include "search/search.h"

#include "editor/editor.h"
#include "search/searchmatch.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>

#define MAX_HITS 2000   /* results shown; the search stops there */
#define DEBOUNCE_MS 300 /* typing pauses this long before a search starts */

typedef struct {
  GSubprocess *proc;
  GDataInputStream *out;
  GCancellable *cancel;
  GRegex *regex;
  char *root; /* the folder searched: result paths are relative to it */
} Run;

static struct {
  App *app;
  GtkEntry *entry, *include, *exclude;
  GtkToggleButton *match_case, *whole_word, *regex, *details;
  GtkWidget *details_box;
  GtkLabel *summary;
  GtkListBox *list;
  guint debounce_id;
  Run *run; /* the search running, NULL when none */
  int hits, files;
  gboolean truncated; /* stopped at MAX_HITS */
  GtkWidget *heading; /* the row of the file whose lines come in */
  char *file;         /* its path */
} search;

static const char *const css =
    ".tcode-search { background: #1f1f1f; }"
    ".tcode-search .field { background: #313131; border: 1px solid #3c3c3c; border-radius: 6px; margin: 8px 10px 0; }"
    ".tcode-search .field:focus-within { border-color: #0078d4; }"
    ".tcode-search .field entry { background: none; border: none; box-shadow: none; outline: none; color: #cccccc;"
    "  min-height: 24px; padding: 0 6px; }"
    ".tcode-search .field button.option { min-height: 0; min-width: 0; padding: 0 4px; margin: 3px 2px;"
    "  border-radius: 4px; border: 1px solid transparent; background: none; box-shadow: none; color: #cccccc;"
    "  font-family: 'Cascadia Code', monospace; font-size: 0.85em; }"
    ".tcode-search .field button.option:hover { background: rgba(90, 93, 94, .5); }"
    ".tcode-search .field button.option:checked { background: rgba(36, 137, 219, .51); border-color: #2488db;"
    "  color: #ffffff; }"
    ".tcode-search .label { color: #9d9d9d; font-size: 0.8em; margin: 6px 10px 0; }"
    ".tcode-search button.details { min-height: 0; min-width: 0; padding: 0 6px; margin: 2px 10px 0;"
    "  background: none; box-shadow: none; color: #cccccc; }"
    ".tcode-search button.details:checked { color: #ffffff; background: rgba(255, 255, 255, .08); }"
    ".tcode-search .summary { color: #9d9d9d; font-size: 0.85em; margin: 8px 6px 4px 10px; }"
    ".tcode-search .summary.error { color: #f14c4c; }"
    ".tcode-search button.tool { min-height: 0; min-width: 0; padding: 0 5px; margin-top: 4px; }"
    ".tcode-search list { background: transparent; }"
    ".tcode-search row { padding: 0 6px 0 28px; margin: 0 4px; min-height: 22px; border-radius: 6px; color: #cccccc; }"
    ".tcode-search row.heading { padding-left: 6px; }"
    ".tcode-search row:hover { background: #2a2d2e; }"
    ".tcode-search row:selected { background: #04395e; }"
    ".tcode-search row .dim { color: #8b8b8b; font-size: 0.9em; }"
    ".tcode-search row .chevron { color: #cccccc; min-width: 14px; }"
    ".tcode-search row .badge { background: #616161; color: #f8f8f8; border-radius: 9px; padding: 0 6px;"
    "  font-size: 0.8em; }";

/* Searching ---------------------------------------------------------------------------------- */

static void run_free(Run *run)
{
  g_clear_object(&run->out);
  g_clear_object(&run->proc);
  g_clear_object(&run->cancel);
  g_clear_pointer(&run->regex, g_regex_unref);
  g_free(run->root);
  g_free(run);
#ifdef __GLIBC__
  malloc_trim(0); /* glibc keeps what a search freed (its results can be large): give it back */
#endif
}

/* Stops the search running; its pending read then ends with an error and frees it. */
static void stop_run(void)
{
  if (!search.run)
    return;
  g_cancellable_cancel(search.run->cancel);
  g_subprocess_force_exit(search.run->proc);
  search.run = NULL;
}

/* The line above the results ("12 results in 3 files"); errors in red. */
static void set_summary(const char *text, gboolean error)
{
  gtk_label_set_text(search.summary, text);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(search.summary), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(search.summary), "error");
}

/* The summary for the results so far. */
static void update_summary(void)
{
  char *text;

  if (search.hits == 0) {
    set_summary(search.run ? "Searching…" : "No results found.", FALSE);
    return;
  }
  text = g_strdup_printf("%d result%s in %d file%s%s", search.hits, search.hits == 1 ? "" : "s", search.files,
                         search.files == 1 ? "" : "s", search.truncated ? " (search stopped there)" : "");
  set_summary(text, FALSE);
  g_free(text);
}

/* Empties the result list. */
static void clear_results(void)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(GTK_WIDGET(search.list))))
    gtk_list_box_remove(search.list, child);
  search.hits = search.files = 0;
  search.truncated = FALSE;
  search.heading = NULL;
  g_clear_pointer(&search.file, g_free);
}

/* A file's heading: chevron, name, its folder dimmed and the number of results. */
static GtkWidget *heading_row(const char *path)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *chevron = gtk_label_new("▾");
  char *name = g_path_get_basename(path), *dir = g_path_get_dirname(path);
  GtkWidget *label = gtk_label_new(name);
  GtkWidget *folder = gtk_label_new(strcmp(dir, ".") == 0 ? "" : dir);
  GtkWidget *badge = gtk_label_new("0");

  gtk_widget_add_css_class(row, "heading");
  gtk_widget_add_css_class(chevron, "chevron");
  gtk_label_set_ellipsize(GTK_LABEL(folder), PANGO_ELLIPSIZE_START);
  gtk_widget_add_css_class(folder, "dim");
  gtk_widget_set_hexpand(folder, TRUE);
  gtk_label_set_xalign(GTK_LABEL(folder), 0);
  gtk_widget_add_css_class(badge, "badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(box), chevron);
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), folder);
  gtk_box_append(GTK_BOX(box), badge);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, path);
  g_object_set_data(G_OBJECT(row), "chevron", chevron);
  g_object_set_data(G_OBJECT(row), "badge", badge);
  g_object_set_data_full(G_OBJECT(row), "lines", g_ptr_array_new(), (GDestroyNotify)g_ptr_array_unref);
  g_free(name);
  g_free(dir);
  return row;
}

/* Folds (hides the lines of) or unfolds a file's results. */
static void set_folded(GtkWidget *heading, gboolean folded)
{
  GPtrArray *lines = g_object_get_data(G_OBJECT(heading), "lines");

  g_object_set_data(G_OBJECT(heading), "folded", GINT_TO_POINTER(folded));
  gtk_label_set_text(g_object_get_data(G_OBJECT(heading), "chevron"), folded ? "▸" : "▾");
  for (guint i = 0; i < lines->len; i++)
    gtk_widget_set_visible(lines->pdata[i], !folded);
}

/* Whether a file's results are folded. */
static gboolean is_folded(GtkWidget *heading)
{
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(heading), "folded"));
}

/* A result: the line with the match marked, under its file's heading. */
static void add_hit(const SearchHit *hit, gpointer data)
{
  Run *run = data;
  GtkWidget *row, *label;
  GPtrArray *lines;
  char *before, *match, *after, *markup, *count, *tip;

  if (search.hits >= MAX_HITS) {
    search.truncated = TRUE;
    return;
  }
  if (!search.file || strcmp(search.file, hit->path) != 0) {
    g_set_str(&search.file, hit->path);
    search.heading = heading_row(hit->path);
    gtk_list_box_append(search.list, search.heading);
    search.files++;
  }
  before = search_snippet(hit->text, hit->start, hit->end, &match, &after);
  if (hit->start == hit->end)
    g_strchug(after);
  markup = g_markup_printf_escaped("%s<span background=\"#EA5C00\" bgalpha=\"40%%\">%s</span>%s", before, match, after);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
  row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
  tip = g_strstrip(g_strdelimit(g_strdup(hit->text), "\t", ' '));
  gtk_widget_set_tooltip_text(row, tip);
  g_object_set_data_full(G_OBJECT(row), "path", g_build_filename(run->root, hit->path, NULL), g_free);
  g_object_set_data(G_OBJECT(row), "line", GINT_TO_POINTER(hit->line));
  g_object_set_data(G_OBJECT(row), "column", GINT_TO_POINTER(hit->column));
  gtk_widget_set_visible(row, !is_folded(search.heading));
  gtk_list_box_append(search.list, row);

  lines = g_object_get_data(G_OBJECT(search.heading), "lines");
  g_ptr_array_add(lines, row);
  count = g_strdup_printf("%u", lines->len);
  gtk_label_set_text(g_object_get_data(G_OBJECT(search.heading), "badge"), count);
  search.hits++;
  g_free(count);
  g_free(tip);
  g_free(markup);
  g_free(before);
  g_free(match);
  g_free(after);
}

/* The search program exited: the summary says how it went. */
static void on_exit(GObject *source, GAsyncResult *result, gpointer data)
{
  Run *run = data;
  gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);

  if (run == search.run) {
    search.run = NULL;
    /* 0: found, 1: nothing found, 2: an error (a regular expression ripgrep cannot read). */
    if (ok && search.hits == 0 && g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
        g_subprocess_get_exit_status(G_SUBPROCESS(source)) > 1)
      set_summary("The search failed (is the regular expression valid?)", TRUE);
    else
      update_summary();
  }
  run_free(run);
}

/* A line of the search program's output: one result, then the next line is read. */
static void on_line(GObject *source, GAsyncResult *result, gpointer data)
{
  Run *run = data;
  gsize len;
  GError *error = NULL;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &len, &error);

  if (run != search.run) { /* stopped: a newer search runs, or none */
    g_clear_error(&error);
    g_free(line);
    run_free(run);
    return;
  }
  if (!line) { /* the end, or the pipe failed: the exit status tells */
    g_clear_error(&error);
    g_subprocess_wait_async(run->proc, run->cancel, on_exit, run);
    return;
  }
  search_parse_line(line, len, run->regex, add_hit, run);
  g_free(line);
  if (search.truncated) {
    stop_run();
    update_summary();
    run_free(run);
    return;
  }
  update_summary();
  g_data_input_stream_read_line_async(run->out, G_PRIORITY_DEFAULT, run->cancel, on_line, run);
}

/* Starts a search for what the panel holds now, stopping the one that runs. */
static void start_search(void)
{
  App *t = search.app;
  SearchQuery q = {
    .text = gtk_editable_get_text(GTK_EDITABLE(search.entry)),
    .match_case = gtk_toggle_button_get_active(search.match_case),
    .whole_word = gtk_toggle_button_get_active(search.whole_word),
    .regex = gtk_toggle_button_get_active(search.regex),
    .include = gtk_editable_get_text(GTK_EDITABLE(search.include)),
    .exclude = gtk_editable_get_text(GTK_EDITABLE(search.exclude)),
  };
  char *rg = g_find_program_in_path("rg");
  GSubprocessLauncher *launcher;
  GError *error = NULL;
  GRegex *regex;
  char **argv;
  Run *run;

  g_clear_handle_id(&search.debounce_id, g_source_remove);
  stop_run();
  clear_results();
  if (!*q.text) {
    set_summary("", FALSE);
    g_free(rg);
    return;
  }
  regex = search_regex(&q, &error);
  if (!regex) {
    set_summary("Invalid regular expression", TRUE);
    g_error_free(error);
    g_free(rg);
    return;
  }
  argv = search_argv(&q, rg != NULL);
  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_cwd(launcher, t->project.root);
  run = g_new0(Run, 1);
  run->regex = regex;
  run->root = g_strdup(t->project.root);
  run->proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv, &error);
  g_object_unref(launcher);
  g_strfreev(argv);
  g_free(rg);
  if (!run->proc) {
    set_summary(error->message, TRUE);
    g_error_free(error);
    run_free(run);
    return;
  }
  run->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(run->proc));
  run->cancel = g_cancellable_new();
  search.run = run;
  update_summary();
  g_data_input_stream_read_line_async(run->out, G_PRIORITY_DEFAULT, run->cancel, on_line, run);
}

/* schedule_search()'s timeout. */
static gboolean on_debounced(gpointer data G_GNUC_UNUSED)
{
  search.debounce_id = 0;
  start_search();
  return G_SOURCE_REMOVE;
}

/* Typing: search once it pauses. */
static void schedule_search(void)
{
  g_clear_handle_id(&search.debounce_id, g_source_remove);
  search.debounce_id = g_timeout_add(DEBOUNCE_MS, on_debounced, NULL);
}

/* The panel ---------------------------------------------------------------------------------- */

static void on_row_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  const char *path = g_object_get_data(G_OBJECT(row), "path");

  if (!path) {
    set_folded(GTK_WIDGET(row), !is_folded(GTK_WIDGET(row)));
    return;
  }
  open_file_at(search.app, path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")),
               GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "column")));
}

/* Folds every file; when all are folded already, unfolds them. */
static void fold_all(void)
{
  gboolean all = TRUE;

  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(search.list)); row;
       row = gtk_widget_get_next_sibling(row))
    if (!g_object_get_data(G_OBJECT(row), "path") && !is_folded(row))
      all = FALSE;
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(search.list)); row;
       row = gtk_widget_get_next_sibling(row))
    if (!g_object_get_data(G_OBJECT(row), "path"))
      set_folded(row, !all);
}

/* The ✕ button: empties the search text and the results. */
static void clear_search(void)
{
  gtk_editable_set_text(GTK_EDITABLE(search.entry), ""); /* the change clears the results */
  start_search();
  gtk_widget_grab_focus(GTK_WIDGET(search.entry));
}

/* The ⋯ button shows or hides the include / exclude fields. */
static void on_details(GtkToggleButton *button, gpointer data G_GNUC_UNUSED)
{
  gtk_widget_set_visible(search.details_box, gtk_toggle_button_get_active(button));
}

/* Down from the text goes to the results. */
static gboolean on_entry_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                             GdkModifierType state G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  if (key != GDK_KEY_Down)
    return FALSE;
  return gtk_widget_child_focus(GTK_WIDGET(search.list), GTK_DIR_TAB_FORWARD);
}

/* An entry that searches as it changes (and at once on Enter). */
static GtkEntry *new_entry(const char *placeholder)
{
  GtkWidget *entry = gtk_entry_new();

  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_widget_set_hexpand(entry, TRUE);
  g_signal_connect_swapped(entry, "changed", G_CALLBACK(schedule_search), NULL);
  g_signal_connect_swapped(entry, "activate", G_CALLBACK(start_search), NULL);
  return GTK_ENTRY(entry);
}

/* A framed field: the entry, with option buttons on its right. */
static GtkWidget *field(GtkEntry *entry)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class(box, "field");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(entry));
  return box;
}

/* An option button (Aa, ab, .*) on the right of a field; flipping it searches again. */
static GtkToggleButton *option(GtkWidget *field, const char *text, const char *tooltip)
{
  GtkWidget *button = gtk_toggle_button_new_with_label(text);

  gtk_widget_add_css_class(button, "option");
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  g_signal_connect_swapped(button, "toggled", G_CALLBACK(start_search), NULL);
  gtk_box_append(GTK_BOX(field), button);
  return GTK_TOGGLE_BUTTON(button);
}

/* A small dimmed label above a field. */
static GtkWidget *small_label(const char *text)
{
  GtkWidget *label = gtk_label_new(text);

  gtk_widget_add_css_class(label, "label");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  return label;
}

/* A flat button next to the summary (refresh, fold all, clear). */
static GtkWidget *tool_button(const char *text, const char *tooltip, GCallback clicked)
{
  GtkWidget *button = gtk_button_new_with_label(text);

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_add_css_class(button, "tool");
  g_signal_connect_swapped(button, "clicked", clicked, NULL);
  return button;
}

GtkWidget *search_panel(App *t)
{
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *query, *details, *head, *scroll = gtk_scrolled_window_new();
  GtkEventController *keys = gtk_event_controller_key_new();

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  search.app = t;
  gtk_widget_add_css_class(panel, "tcode-search");

  search.entry = new_entry("Search");
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), NULL);
  gtk_widget_add_controller(GTK_WIDGET(search.entry), keys);
  query = field(search.entry);
  search.match_case = option(query, "Aa", "Match Case");
  search.whole_word = option(query, "ab", "Match Whole Word");
  search.regex = option(query, ".*", "Use Regular Expression");
  gtk_box_append(GTK_BOX(panel), query);

  search.details = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("⋯"));
  gtk_widget_add_css_class(GTK_WIDGET(search.details), "details");
  gtk_widget_set_focusable(GTK_WIDGET(search.details), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(search.details), "Toggle Search Details");
  gtk_widget_set_halign(GTK_WIDGET(search.details), GTK_ALIGN_END);
  g_signal_connect(search.details, "toggled", G_CALLBACK(on_details), NULL);
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(search.details));

  search.details_box = details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  search.include = new_entry("e.g. *.c, src/**/include");
  search.exclude = new_entry("e.g. build, *.o");
  gtk_box_append(GTK_BOX(details), small_label("files to include"));
  gtk_box_append(GTK_BOX(details), field(search.include));
  gtk_box_append(GTK_BOX(details), small_label("files to exclude"));
  gtk_box_append(GTK_BOX(details), field(search.exclude));
  gtk_widget_set_visible(details, FALSE);
  gtk_box_append(GTK_BOX(panel), details);

  head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  search.summary = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(search.summary), "summary");
  gtk_label_set_xalign(search.summary, 0);
  gtk_label_set_wrap(search.summary, TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(search.summary), TRUE);
  gtk_box_append(GTK_BOX(head), GTK_WIDGET(search.summary));
  gtk_box_append(GTK_BOX(head), tool_button("⟳", "Refresh", G_CALLBACK(start_search)));
  gtk_box_append(GTK_BOX(head), tool_button("⊟", "Collapse / Expand All", G_CALLBACK(fold_all)));
  gtk_box_append(GTK_BOX(head), tool_button("✕", "Clear Search Results", G_CALLBACK(clear_search)));
  gtk_box_append(GTK_BOX(panel), head);

  search.list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(search.list, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(search.list, TRUE);
  g_signal_connect(search.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(search.list));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(panel), scroll);
  return panel;
}

/* The keyboard goes to the search text, all of it selected to type over. */
void search_focus(void)
{
  gtk_widget_grab_focus(GTK_WIDGET(search.entry));
  gtk_editable_select_region(GTK_EDITABLE(search.entry), 0, -1);
}

/* Another project: the same search there. */
void search_reset(void)
{
  if (search.entry)
    start_search();
}
