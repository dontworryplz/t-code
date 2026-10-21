/*
 * The Run and Debug panel (see debugview.h). A pause asks gdb for the shown frame's scopes, then each
 * scope's variables (Arguments, then Locals; not Registers); a variable with a variablesReference has
 * children, asked for when its row is shown, and at once when gdb gave no value for it (an array, a
 * struct): its value is then made from theirs, "{70, 85, 90}". Every answer carries the pause it was
 * asked in (generation): answers from before a step are dropped.
 */
#include "debug/debugview.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "editor/editor.h"
#include "lsp/json.h"

#include <string.h>

#define PREVIEW_ITEMS 8 /* children shown in a value made from them: "{1, 2, 3, …}" */

static const char *const css =
    ".tcode-debug-view { background: var(--tc-panel); }"
    ".tcode-debug-view .section { font-size: 0.8em; font-weight: bold; color: var(--tc-fg); padding: 4px 10px; }"
    ".tcode-debug-view .note { color: var(--tc-fg-dim); margin: 10px; }"
    ".tcode-debug-view listview, .tcode-debug-view list { background: none; }"
    ".tcode-debug-view .name { color: var(--tc-variable); }"
    ".tcode-debug-view .value { color: var(--tc-fg); }"
    ".tcode-debug-view .value.changed { color: var(--tc-warning); font-weight: bold; }"
    ".tcode-debug-view .where { color: var(--tc-fg-dim); font-size: 0.9em; }"
    ".tcode-debug-view row.no-source .function { color: var(--tc-fg-faint); }";

/* A variable ------------------------------------------------------------------------------------ */

struct _TcodeVar {
  GObject parent;
  char *name;
  char *value;
  char *key;        /* where it is, to compare with the last pause: "main/p/x" */
  gint64 reference; /* gdb's variablesReference: > 0 when it has children */
  gboolean changed; /* its value is not the last pause's */
  gboolean asked;   /* its children were asked for */
  gboolean preview; /* its value is made from its children's */
  GListStore *children;
};

#define TCODE_TYPE_VAR (tcode_var_get_type())
G_DECLARE_FINAL_TYPE(TcodeVar, tcode_var, TCODE, VAR, GObject)
G_DEFINE_FINAL_TYPE(TcodeVar, tcode_var, G_TYPE_OBJECT)

static guint value_changed; /* the "value-changed" signal: a value made from the children came */

static void tcode_var_finalize(GObject *object)
{
  TcodeVar *self = TCODE_VAR(object);

  g_free(self->name);
  g_free(self->value);
  g_free(self->key);
  g_clear_object(&self->children);
  G_OBJECT_CLASS(tcode_var_parent_class)->finalize(object);
}

static void tcode_var_class_init(TcodeVarClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_var_finalize;
  value_changed =
      g_signal_new("value-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void tcode_var_init(TcodeVar *self G_GNUC_UNUSED)
{
}

/* The panel ------------------------------------------------------------------------------------- */

static struct {
  App *app;
  GtkWidget *note;  /* instead of the lists: not debugging, running */
  GtkWidget *lists; /* Variables over Call Stack */
  GListStore *root; /* the shown frame's variables */
  GtkListBox *stack;
  JsonNode *frames;    /* the last pause's (a copy) */
  int shown;           /* the frame whose variables are listed */
  gboolean all_frames; /* the frames without source are listed one by one, not as one row */
  guint generation;    /* counts pauses and frame changes: older answers are dropped */
  GHashTable *last;    /* key -> value at the last pause */
  GHashTable *now;     /* the same for this one */
} view;

/* What an answer was asked for. */
typedef struct {
  guint generation;
  TcodeVar *parent; /* NULL: the frame's top level */
  char *prefix;     /* the keys' start */
} Asked;

static Asked *asked_new(TcodeVar *parent, const char *prefix)
{
  Asked *a = g_new0(Asked, 1);

  a->generation = view.generation;
  a->parent = parent ? g_object_ref(parent) : NULL;
  a->prefix = g_strdup(prefix);
  return a;
}

static void asked_free(gpointer data)
{
  Asked *a = data;

  if (a->parent)
    g_object_unref(a->parent);
  g_free(a->prefix);
  g_free(a);
}

/* Shows the lists, or a note instead (NULL: the lists). */
static void show_note(const char *note)
{
  if (!view.note)
    return;
  gtk_widget_set_visible(view.note, note != NULL);
  gtk_widget_set_visible(view.lists, note == NULL);
  if (note)
    gtk_label_set_text(GTK_LABEL(view.note), note);
}

/* A value made from the children's: "{70, 85, 90}" (nested ones "{…}"). */
static char *preview_of(GListStore *children)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(children));
  GString *text = g_string_new("{");

  for (guint i = 0; i < n && i < PREVIEW_ITEMS; i++) {
    TcodeVar *child = g_list_model_get_item(G_LIST_MODEL(children), i);
    g_string_append_printf(text, "%s%s", i ? ", " : "", *child->value ? child->value : "{…}");
    g_object_unref(child);
  }
  if (n > PREVIEW_ITEMS)
    g_string_append(text, ", …");
  g_string_append(text, "}");
  return g_string_free(text, FALSE);
}

/* Whether a value differs from the last pause's at key (and remembers it for the next one). */
static gboolean changed(const char *key, const char *value)
{
  const char *before = g_hash_table_lookup(view.last, key);

  g_hash_table_replace(view.now, g_strdup(key), g_strdup(value));
  return before && strcmp(before, value) != 0;
}

static void ask_children(TcodeVar *var);

/* gdb's variables for a scope or a variable: rows. */
static void on_variables(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  JsonNode *list = json_at(body, "variables");
  GListStore *into = a->parent ? a->parent->children : view.root;
  guint n = list && JSON_NODE_HOLDS_ARRAY(list) ? json_array_get_length(json_node_get_array(list)) : 0;

  if (a->generation != view.generation)
    return;
  if (error && a->parent) { /* a NULL pointer's target: say why it has none */
    TcodeVar *note = g_object_new(TCODE_TYPE_VAR, NULL);
    note->name = g_strdup("");
    note->value = g_strdup(error);
    note->key = g_strdup("");
    g_list_store_append(into, note);
    g_object_unref(note);
  }
  for (guint i = 0; i < n; i++) {
    JsonNode *item = json_array_get_element(json_node_get_array(list), i);
    const char *name = json_string_at(item, "name"), *value = json_string_at(item, "value");
    TcodeVar *var = g_object_new(TCODE_TYPE_VAR, NULL);
    gboolean index = name && g_ascii_isdigit(name[0]); /* an array's element */
    var->name = index ? g_strdup_printf("[%s]", name) : g_strdup(name ? name : "?");
    var->value = g_strdup(value ? value : "");
    var->reference = json_int_at(item, "variablesReference", 0);
    var->key = g_strdup_printf("%s/%s", a->prefix, var->name);
    if (var->reference > 0)
      var->children = g_list_store_new(TCODE_TYPE_VAR);
    if (*var->value)
      var->changed = changed(var->key, var->value);
    else if (var->reference > 0) { /* an array or a struct: its value comes from its children */
      var->preview = TRUE;
      ask_children(var);
    }
    g_list_store_append(into, var);
    g_object_unref(var);
  }
  if (a->parent && a->parent->preview) {
    g_free(a->parent->value);
    a->parent->value = preview_of(a->parent->children);
    a->parent->changed = changed(a->parent->key, a->parent->value);
    g_signal_emit(a->parent, value_changed, 0);
  }
}

static void ask_children(TcodeVar *var)
{
  Dap *dap = debug_dap(view.app);
  char *args;

  if (var->asked || var->reference <= 0 || !dap)
    return;
  var->asked = TRUE;
  args = g_strdup_printf("{\"variablesReference\":%" G_GINT64_FORMAT "}", var->reference);
  dap_request_full(dap, "variables", args, on_variables, asked_new(var, var->key), asked_free);
  g_free(args);
}

/* The frame's scopes: Arguments and Locals are listed (Registers are for later). */
static void on_scopes(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  JsonNode *scopes = json_at(body, "scopes");
  guint n = scopes && JSON_NODE_HOLDS_ARRAY(scopes) ? json_array_get_length(json_node_get_array(scopes)) : 0;
  Dap *dap = debug_dap(view.app);

  if (a->generation != view.generation || !dap)
    return;
  if (error || n == 0)
    show_note("No variables here (no debugging information for this function).");
  for (int pass = 0; pass < 2; pass++) /* arguments first */
    for (guint i = 0; i < n; i++) {
      JsonNode *scope = json_array_get_element(json_node_get_array(scopes), i);
      const char *hint = json_string_at(scope, "presentationHint");
      if (g_strcmp0(hint, pass == 0 ? "arguments" : "locals") != 0)
        continue;
      char *args =
          g_strdup_printf("{\"variablesReference\":%" G_GINT64_FORMAT "}", json_int_at(scope, "variablesReference", 0));
      dap_request_full(dap, "variables", args, on_variables, asked_new(NULL, a->prefix), asked_free);
      g_free(args);
    }
}

static GtkListBoxRow *row_of_frame(int index);

/* The frame at index (of view.frames) is listed: its variables are asked for. */
static void list_frame(int index)
{
  JsonNode *frame = view.frames ? json_array_get_element(json_node_get_array(view.frames), index) : NULL;
  Dap *dap = debug_dap(view.app);
  const char *function = json_string_at(frame, "name");
  char *args;

  view.generation++;
  view.shown = index;
  g_list_store_remove_all(view.root);
  if (!frame || !dap)
    return;
  args = g_strdup_printf("{\"frameId\":%" G_GINT64_FORMAT "}", json_int_at(frame, "id", 0));
  dap_request_full(dap, "scopes", args, on_scopes, asked_new(NULL, function ? function : "?"), asked_free);
  g_free(args);
  gtk_list_box_select_row(view.stack, row_of_frame(index));
}

/* Variables' rows ------------------------------------------------------------------------------- */

static GListModel *children_of(gpointer item, gpointer data G_GNUC_UNUSED)
{
  TcodeVar *var = item;

  if (!var->children)
    return NULL;
  ask_children(var); /* asked for when the row is shown, so its arrow knows */
  return G_LIST_MODEL(g_object_ref(var->children));
}

static void setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkWidget *expander = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *name = gtk_label_new(NULL), *equals = gtk_label_new(" = "), *value = gtk_label_new(NULL);

  gtk_widget_add_css_class(name, "name");
  gtk_widget_add_css_class(value, "value");
  gtk_label_set_xalign(GTK_LABEL(value), 0);
  gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable(GTK_LABEL(value), FALSE);
  gtk_widget_set_hexpand(value, TRUE);
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), equals);
  gtk_box_append(GTK_BOX(box), value);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
  gtk_list_item_set_child(item, expander);
}

/* The row shows var's value now. */
static void show_value(TcodeVar *var, GtkWidget *box)
{
  GtkWidget *name = gtk_widget_get_first_child(box);
  GtkWidget *equals = gtk_widget_get_next_sibling(name), *value = gtk_widget_get_last_child(box);

  gtk_label_set_text(GTK_LABEL(name), var->name);
  gtk_widget_set_visible(equals, *var->name != '\0');
  gtk_label_set_text(GTK_LABEL(value), var->value);
  gtk_widget_set_tooltip_text(value, *var->value ? var->value : NULL);
  if (var->changed)
    gtk_widget_add_css_class(value, "changed");
  else
    gtk_widget_remove_css_class(value, "changed");
}

static void bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkTreeListRow *row = gtk_list_item_get_item(item);
  GtkWidget *expander = gtk_list_item_get_child(item);
  GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
  TcodeVar *var = gtk_tree_list_row_get_item(row);

  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
  show_value(var, box);
  g_signal_connect_object(var, "value-changed", G_CALLBACK(show_value), box, G_CONNECT_DEFAULT);
  g_object_set_data_full(G_OBJECT(item), "var", var, g_object_unref);
}

static void unbind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(gtk_list_item_get_child(item)));
  TcodeVar *var = g_object_get_data(G_OBJECT(item), "var");

  if (var)
    g_signal_handlers_disconnect_by_data(var, box);
  g_object_set_data(G_OBJECT(item), "var", NULL);
}

/* Call Stack ------------------------------------------------------------------------------------ */

/* Whether frame has a file on disk to show. */
static gboolean has_source(JsonNode *frame)
{
  const char *path = json_string_at(frame, "source.path");

  return path && g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

static GtkWidget *stack_row(const char *function, const char *where, int index)
{
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *name = gtk_label_new(function), *place = gtk_label_new(where);

  gtk_widget_add_css_class(name, "function");
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_widget_add_css_class(place, "where");
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), place);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data(G_OBJECT(row), "frame", GINT_TO_POINTER(index)); /* -1: the hidden frames' row */
  return row;
}

/* The frames, the program's own with their file and line. Frames without a file (the C library's,
 * ASan's) in a row go in one dim row, which opens them. */
static void fill_stack(void)
{
  guint n = view.frames ? json_array_get_length(json_node_get_array(view.frames)) : 0;

  gtk_list_box_remove_all(view.stack);
  for (guint i = 0; i < n; i++) {
    JsonNode *frame = json_array_get_element(json_node_get_array(view.frames), i);
    const char *function = json_string_at(frame, "name"), *path = json_string_at(frame, "source.path");
    guint run = 0;
    GtkWidget *row;

    while (!view.all_frames && i + run < n &&
           !has_source(json_array_get_element(json_node_get_array(view.frames), i + run)))
      run++;
    if (run > 1) {
      char *text = g_strdup_printf("%u frames in libraries", run);
      row = stack_row(text, "show", -1);
      gtk_widget_set_tooltip_text(row, "Functions of the C library or of ASan, without source: click to list them");
      gtk_widget_add_css_class(row, "no-source");
      gtk_list_box_append(view.stack, row);
      g_free(text);
      i += run - 1;
      continue;
    }
    if (has_source(frame)) {
      char *file = g_path_get_basename(path);
      char *where = g_strdup_printf("%s:%d", file, (int)json_int_at(frame, "line", 0));
      row = stack_row(function ? function : "?", where, (int)i);
      gtk_widget_set_tooltip_text(row, path);
      g_free(where);
      g_free(file);
    } else {
      row = stack_row(function ? function : "?", "", (int)i);
      gtk_widget_set_tooltip_text(row, "No source: a library without debugging information");
      gtk_widget_add_css_class(row, "no-source");
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    }
    gtk_list_box_append(view.stack, row);
  }
}

/* The row of frame index, or NULL (hidden). */
static GtkListBoxRow *row_of_frame(int index)
{
  for (GtkListBoxRow *row = gtk_list_box_get_row_at_index(view.stack, 0); row;
       row = GTK_LIST_BOX_ROW(gtk_widget_get_next_sibling(GTK_WIDGET(row))))
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "frame")) == index)
      return row;
  return NULL;
}

/* A click on a frame: its line in the editor, its variables in the list; on the hidden frames' row:
 * they show. */
static void on_frame_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "frame"));
  JsonNode *frame = view.frames && index >= 0 ? json_array_get_element(json_node_get_array(view.frames), index) : NULL;
  const char *path = json_string_at(frame, "source.path");

  if (index < 0) {
    view.all_frames = TRUE;
    fill_stack();
    gtk_list_box_select_row(view.stack, row_of_frame(view.shown));
    return;
  }
  if (!frame || !path)
    return;
  open_file_at(view.app, path, (int)json_int_at(frame, "line", 1), 1);
  if (index != view.shown)
    list_frame(index);
}

/* From the debugger -------------------------------------------------------------------------- */

void debug_view_paused(App *t G_GNUC_UNUSED, JsonNode *frames, int shown)
{
  GHashTable *older = view.last;

  if (!view.root)
    return;
  view.last = view.now; /* changed values are those that differ from the last pause's */
  view.now = older;
  g_hash_table_remove_all(view.now);
  g_clear_pointer(&view.frames, json_node_unref);
  view.frames = frames && JSON_NODE_HOLDS_ARRAY(frames) ? json_node_copy(frames) : NULL;
  view.all_frames = FALSE;
  fill_stack();
  show_note(NULL);
  list_frame(MAX(shown, 0));
}

void debug_view_running(App *t G_GNUC_UNUSED)
{
  if (!view.root)
    return;
  view.generation++;
  g_list_store_remove_all(view.root);
  gtk_list_box_remove_all(view.stack);
  show_note("Running… The variables show here when the program pauses: at a breakpoint, after a step, or with "
            "Pause (F6).");
}

void debug_view_ended(App *t G_GNUC_UNUSED)
{
  if (!view.root)
    return;
  view.generation++;
  g_list_store_remove_all(view.root);
  gtk_list_box_remove_all(view.stack);
  g_clear_pointer(&view.frames, json_node_unref);
  g_hash_table_remove_all(view.last);
  g_hash_table_remove_all(view.now);
  show_note("Not debugging.\n\nClick left of a line number (or F9) to put a breakpoint there, then F5: the program "
            "pauses at that line, and its variables show here.");
}

char *debug_view_variables_text(void)
{
  GString *text = g_string_new("");
  guint n = view.root ? g_list_model_get_n_items(G_LIST_MODEL(view.root)) : 0;

  for (guint i = 0; i < n; i++) {
    TcodeVar *var = g_list_model_get_item(G_LIST_MODEL(view.root), i);
    g_string_append_printf(text, "%s = %s%s\n", var->name, var->value, var->changed ? "*" : "");
    g_object_unref(var);
  }
  return g_string_free(text, FALSE);
}

char *debug_view_stack_text(void)
{
  GString *text = g_string_new("");

  for (GtkWidget *row = view.stack ? gtk_widget_get_first_child(GTK_WIDGET(view.stack)) : NULL; row;
       row = gtk_widget_get_next_sibling(row)) {
    GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    g_string_append_printf(text, "%s %s\n", gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(box))),
                           gtk_label_get_text(GTK_LABEL(gtk_widget_get_last_child(box))));
  }
  return g_string_free(text, FALSE);
}

static GtkWidget *heading(const char *text)
{
  GtkWidget *label = gtk_label_new(text);

  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "section");
  return label;
}

static void on_view_destroyed(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  view.note = view.lists = NULL;
  view.stack = NULL;
  g_clear_object(&view.root);
  g_clear_pointer(&view.frames, json_node_unref);
  g_clear_pointer(&view.last, g_hash_table_destroy);
  g_clear_pointer(&view.now, g_hash_table_destroy);
}

GtkWidget *debug_view(App *t)
{
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *vars_scroll = gtk_scrolled_window_new(), *stack_scroll = gtk_scrolled_window_new();
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  GtkTreeListModel *tree;
  GtkWidget *list;

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  view.app = t;
  view.root = g_list_store_new(TCODE_TYPE_VAR);
  view.last = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  view.now = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  gtk_widget_add_css_class(panel, "tcode-debug-view");

  tree = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(view.root)), FALSE, FALSE, children_of, NULL, NULL);
  g_signal_connect(factory, "setup", G_CALLBACK(setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), NULL);
  list = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(tree))), factory);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vars_scroll), list);
  gtk_widget_set_vexpand(vars_scroll, TRUE);
  gtk_box_append(GTK_BOX(top), heading("VARIABLES"));
  gtk_box_append(GTK_BOX(top), vars_scroll);

  view.stack = GTK_LIST_BOX(gtk_list_box_new());
  g_signal_connect(view.stack, "row-activated", G_CALLBACK(on_frame_activated), NULL);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(stack_scroll), GTK_WIDGET(view.stack));
  gtk_widget_set_vexpand(stack_scroll, TRUE);
  gtk_box_append(GTK_BOX(bottom), heading("CALL STACK"));
  gtk_box_append(GTK_BOX(bottom), stack_scroll);

  gtk_paned_set_start_child(GTK_PANED(paned), top);
  gtk_paned_set_end_child(GTK_PANED(paned), bottom);
  gtk_paned_set_position(GTK_PANED(paned), 360);
  gtk_widget_set_vexpand(paned, TRUE);
  view.lists = paned;
  view.note = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(view.note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(view.note), 0);
  gtk_widget_set_valign(view.note, GTK_ALIGN_START);
  gtk_widget_add_css_class(view.note, "note");
  gtk_box_append(GTK_BOX(panel), view.note);
  gtk_box_append(GTK_BOX(panel), paned);
  g_signal_connect(panel, "destroy", G_CALLBACK(on_view_destroyed), NULL);
  debug_view_ended(t);
  return panel;
}
