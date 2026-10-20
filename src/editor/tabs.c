/*
 * The tabs and the two tab groups (see tabs.h). What an editor tab does is in editor.c; its page
 * is built in editorview.c.
 */
#include "editor/tabs.h"

#include "editor/diffgutter.h"
#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/preview.h"
#include "files/files.h"
#include "git/compare.h"
#include "window/statusbar.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>

/* The page went (its tab closed, or t-code quits): frees everything the tab holds. */
static void tab_free(Tab *tab)
{
  if (tab->preview)
    preview_free(tab->preview);
  editor_view_free(tab->view);
  g_free(tab->compare.with);
  g_free(tab->compare.title);
  g_free(tab->path);
  g_free(tab);
#ifdef __GLIBC__
  malloc_trim(0); /* glibc keeps what the tab freed: give it back to the system */
#endif
}

/* add_tab() hangs the Tab on its page. */
Tab *tab_of(GtkWidget *page)
{
  return g_object_get_data(G_OBJECT(page), "t-code-tab");
}

int tab_count(App *t)
{
  return gtk_notebook_get_n_pages(t->editors.groups[0]) + gtk_notebook_get_n_pages(t->editors.groups[1]);
}

Tab *tab_nth(App *t, int index)
{
  int first = gtk_notebook_get_n_pages(t->editors.groups[0]);

  return index < first ? tab_of(gtk_notebook_get_nth_page(t->editors.groups[0], index))
                       : tab_of(gtk_notebook_get_nth_page(t->editors.groups[1], index - first));
}

/* The tab's notebook page: its own, or the comparison holding both sides. */
static GtkWidget *frame_of(Tab *tab)
{
  return tab->frame ? tab->frame : tab->page;
}

int tab_group(Tab *tab)
{
  return gtk_notebook_page_num(tab->app->editors.groups[1], frame_of(tab)) >= 0 ? 1 : 0;
}

Tab *current_tab(App *t)
{
  for (int k = 0; k < GROUPS; k++) {
    GtkNotebook *group = t->editors.groups[k == 0 ? t->editors.active : 1 - t->editors.active];
    int current = gtk_notebook_get_current_page(group);
    if (current >= 0)
      return tab_of(gtk_notebook_get_nth_page(group, current));
  }
  return NULL;
}

Tab *find_tab(App *t, TabKind kind, const char *path)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == kind && g_strcmp0(tab->path, path) == 0)
      return tab;
  }
  return NULL;
}

gboolean any_unsaved(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->modified)
      return TRUE;
  return FALSE;
}

/* The widget that takes the keyboard in a tab: an editor's text, not the box around it. */
static GtkWidget *focus_widget(Tab *tab)
{
  return tab->source ? GTK_WIDGET(tab->source) : tab->page;
}

void focus_current_tab(App *t)
{
  Tab *tab = current_tab(t);

  if (tab)
    gtk_widget_grab_focus(focus_widget(tab));
  else
    files_focus(t);
}

void focus_group(App *t, int group)
{
  if (group == 1 && !gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1])))
    return;
  t->editors.active = group;
  focus_current_tab(t);
}

void select_tab(Tab *tab)
{
  App *t = tab->app;
  int group = tab_group(tab);

  gtk_notebook_set_current_page(t->editors.groups[group],
                                gtk_notebook_page_num(t->editors.groups[group], frame_of(tab)));
  t->editors.active = group;
  gtk_widget_grab_focus(focus_widget(tab));
}

void step_tab(App *t, int step)
{
  Tab *tab = current_tab(t);

  if (!tab)
    return;
  GtkNotebook *group = t->editors.groups[tab_group(tab)];
  int n = gtk_notebook_get_n_pages(group);
  gtk_notebook_set_current_page(group, (gtk_notebook_get_current_page(group) + step + n) % n);
  focus_current_tab(t);
}

/* A comparison is taken apart before its sides go: neither may reach the other once freed. */
static void unpair(Tab *tab)
{
  if (!tab->compare.old_side)
    return;
  unsync_scroll(tab);
  diff_gutter_pair(tab, NULL);
}

void remove_tab(Tab *tab)
{
  App *t = tab->app;
  GtkNotebook *group = t->editors.groups[tab_group(tab)];

  unpair(tab);
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, frame_of(tab))); /* both sides of a comparison */
  focus_current_tab(t);
}

/* Editors ask about unsaved changes first. */
static void close_tab(Tab *tab)
{
  if (tab->kind != TAB_EDITOR) {
    remove_tab(tab);
    return;
  }
  select_tab(tab);
  quit_editor(tab);
}

void close_all_tabs(App *t)
{
  t->closing = TRUE; /* quiet while the tabs go */
  for (int g = 0; g < GROUPS; g++)
    while (gtk_notebook_get_n_pages(t->editors.groups[g]) > 0) {
      unpair(tab_of(gtk_notebook_get_nth_page(t->editors.groups[g], 0)));
      gtk_notebook_remove_page(t->editors.groups[g], 0);
    }
  t->closing = FALSE;
}

/* Labels ------------------------------------------------------------------------------------- */

void update_tab_label(Tab *tab)
{
  char *name;

  if (!tab->label) /* a comparison's right side: the left one's label is the page's */
    return;
  name = tab->compare.title ? g_strdup(tab->compare.title) : g_path_get_basename(tab->path);
  if (tab->compare.old_side) { /* "editor.c ↔ a86a89b" */
    const char *other = tab->compare.old_side->compare.title ? tab->compare.old_side->compare.title : "";
    const char *at = strstr(other, " @ ");
    char *both = g_strdup_printf("%s ↔ %s", name, at ? at + 3 : other);
    g_free(name);
    name = both;
  }
  char *text = tab->kind == TAB_PREVIEW ? g_strdup_printf("Preview %s", name)
                                        : g_strdup_printf("%s%s", tab->modified ? "● " : "", name);

  gtk_label_set_text(tab->label, text);
  if (tab->temporary)
    gtk_widget_add_css_class(GTK_WIDGET(tab->label), "temporary"); /* in italics */
  else
    gtk_widget_remove_css_class(GTK_WIDGET(tab->label), "temporary");
  gtk_widget_set_tooltip_text(gtk_widget_get_parent(GTK_WIDGET(tab->label)), tab->path);
  g_free(text);
  g_free(name);
}

/* A middle click on a tab's label closes it, like in a browser. */
static void on_tab_middle_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press G_GNUC_UNUSED,
                                double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, Tab *tab)
{
  close_tab(tab);
}

/* A double click on a temporary tab's label keeps it, like in VS Code. */
static void on_tab_double_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press, double x G_GNUC_UNUSED,
                                double y G_GNUC_UNUSED, Tab *tab)
{
  if (n_press == 2)
    keep_tab(tab);
}

/* A tab's label: its name (update_tab_label fills it) and a close button. */
static GtkWidget *tab_label(Tab *tab)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  GtkGesture *middle = gtk_gesture_click_new(), *twice = gtk_gesture_click_new();

  tab->label = GTK_LABEL(gtk_label_new(NULL));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->label));
  gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
  gtk_widget_set_focusable(close, FALSE);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(close_tab), tab);
  gtk_box_append(GTK_BOX(box), close);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
  g_signal_connect(middle, "pressed", G_CALLBACK(on_tab_middle_click), tab);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(middle));
  g_signal_connect(twice, "pressed", G_CALLBACK(on_tab_double_click), tab);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(twice));
  return box;
}

/* Adding and moving -------------------------------------------------------------------------- */

/* Puts a page into a group at position (-1: the end) and shows it; tabs can be dragged to
 * reorder them and to the other group. */
static void insert_page(GtkNotebook *group, GtkWidget *page, GtkWidget *label, int position)
{
  int index = gtk_notebook_insert_page(group, page, label, position);

  gtk_notebook_set_tab_reorderable(group, page, TRUE);
  gtk_notebook_set_tab_detachable(group, page, TRUE); /* drag between the two groups */
  gtk_notebook_set_current_page(group, index);
}

/* Adds page as a tab of the active group, right after its current tab, and switches to it. */
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path)
{
  GtkNotebook *group = t->editors.groups[t->editors.active];
  Tab *tab = g_new0(Tab, 1);
  GtkWidget *label;

  tab->app = t;
  tab->kind = kind;
  tab->page = page;
  tab->path = g_strdup(path);
  g_object_set_data_full(G_OBJECT(page), "t-code-tab", tab, (GDestroyNotify)tab_free);
  label = tab_label(tab);
  update_tab_label(tab);
  insert_page(group, page, label, gtk_notebook_get_current_page(group) + 1);
  return tab;
}

Tab *temporary_tab(App *t, int group)
{
  for (int i = 0; i < gtk_notebook_get_n_pages(t->editors.groups[group]); i++) {
    Tab *tab = tab_of(gtk_notebook_get_nth_page(t->editors.groups[group], i));
    if (tab->temporary)
      return tab;
  }
  return NULL;
}

void keep_tab(Tab *tab)
{
  if (!tab->temporary)
    return;
  tab->temporary = FALSE;
  update_tab_label(tab);
}

int tab_position(Tab *tab)
{
  return gtk_notebook_page_num(tab->app->editors.groups[tab_group(tab)], frame_of(tab));
}

void set_tab_position(Tab *tab, int position)
{
  gtk_notebook_reorder_child(tab->app->editors.groups[tab_group(tab)], frame_of(tab), position);
}

/* The two sides share the width until the user moves the handle. */
static void on_max_position(GtkPaned *paned, GParamSpec *pspec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  int max = 0;

  g_object_get(paned, "max-position", &max, NULL);
  if (max <= 0)
    return;
  gtk_paned_set_position(paned, max / 2);
  g_signal_handlers_disconnect_by_func(paned, on_max_position, NULL);
}

void join_tabs(Tab *left, Tab *right)
{
  App *t = left->app;
  GtkNotebook *group = t->editors.groups[tab_group(left)], *right_group = t->editors.groups[tab_group(right)];
  int position = tab_position(left);
  GtkWidget *label = g_object_ref(gtk_notebook_get_tab_label(group, left->page));
  GtkPaned *paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));

  g_object_ref(left->page);
  g_object_ref(right->page);
  gtk_notebook_remove_page(right_group, gtk_notebook_page_num(right_group, right->page));
  right->label = NULL; /* went with its notebook page */
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, left->page));
  gtk_paned_set_start_child(paned, left->page);
  gtk_paned_set_end_child(paned, right->page);
  gtk_paned_set_shrink_start_child(paned, FALSE);
  gtk_paned_set_shrink_end_child(paned, FALSE);
  g_signal_connect(paned, "notify::max-position", G_CALLBACK(on_max_position), NULL);
  g_object_set_data(G_OBJECT(paned), "t-code-tab", left); /* tab_of(): the left side */
  left->frame = GTK_WIDGET(paned);
  left->compare.old_side = right;
  insert_page(group, left->frame, label, position);
  update_tab_label(left);
  g_object_unref(label);
  g_object_unref(right->page);
  g_object_unref(left->page);
}

void part_tabs(Tab *left)
{
  App *t = left->app;
  GtkNotebook *group;
  GtkWidget *label;
  int position;

  if (!left->compare.old_side)
    return;
  group = t->editors.groups[tab_group(left)];
  position = tab_position(left);
  label = g_object_ref(gtk_notebook_get_tab_label(group, left->frame));
  unpair(left);
  g_object_ref(left->page);
  gtk_paned_set_start_child(GTK_PANED(left->frame), NULL);
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, left->frame)); /* the right side goes with it */
  left->frame = NULL;
  left->compare.old_side = NULL;
  insert_page(group, left->page, label, position);
  update_tab_label(left);
  g_object_unref(left->page);
  g_object_unref(label);
}

/* Moves a tab (with its editor) to the end of the other group. */
void move_tab(Tab *tab, int to)
{
  App *t = tab->app;
  int from = tab_group(tab);
  GtkWidget *frame = frame_of(tab), *label = gtk_notebook_get_tab_label(t->editors.groups[from], frame);

  if (from == to)
    return;
  g_object_ref(frame);
  g_object_ref(label);
  gtk_notebook_remove_page(t->editors.groups[from], gtk_notebook_page_num(t->editors.groups[from], frame));
  insert_page(t->editors.groups[to], frame, label, -1);
  g_object_unref(label);
  g_object_unref(frame);
}

/* Groups ------------------------------------------------------------------------------------- */

void update_editor_area(App *t)
{
  gboolean split = gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1]));

  gtk_stack_set_visible_child_name(t->editors.area, tab_count(t) > 0 || split ? "tabs" : "empty");
}

void set_split(App *t, gboolean split)
{
  GtkWidget *side = GTK_WIDGET(t->editors.groups[1]);

  if (!split) {
    while (gtk_notebook_get_n_pages(t->editors.groups[1]) > 0)
      move_tab(tab_of(gtk_notebook_get_nth_page(t->editors.groups[1], 0)), 0);
    t->editors.active = 0;
  }
  if (split != gtk_widget_get_visible(side)) {
    gtk_widget_set_visible(side, split);
    int width = gtk_widget_get_width(GTK_WIDGET(t->editors.split));
    if (split && width > 0)
      gtk_paned_set_position(t->editors.split, width / 2);
    if (split)
      t->editors.active = 1;
  }
  if (t->editors.split_button && gtk_toggle_button_get_active(t->editors.split_button) != split)
    gtk_toggle_button_set_active(t->editors.split_button, split);
  update_editor_area(t);
}

/* The second group goes away when its last tab closes. */
static gboolean unsplit_if_empty(gpointer data)
{
  App *t = data;

  if (!t->closing && gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1])) &&
      gtk_notebook_get_n_pages(t->editors.groups[1]) == 0)
    set_split(t, FALSE);
  return G_SOURCE_REMOVE;
}

/* A tab was added to or removed from a group. */
static void on_tabs_changed(GtkNotebook *group, GtkWidget *child G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
  if (t->closing)
    return;
  update_editor_area(t);
  update_status(t);
  if (group == t->editors.groups[1] && gtk_notebook_get_n_pages(group) == 0)
    g_idle_add(unsplit_if_empty, t);
}

/* Another tab became the current one of a group: that group is the active one now. */
static void on_tab_switched(GtkNotebook *group, GtkWidget *page G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
  t->editors.active = group == t->editors.groups[1];
  update_status(t);
}

/* A click into a group makes it the active one (new tabs go there). */
static void on_group_focus(GtkEventControllerFocus *focus, App *t)
{
  GtkWidget *group = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(focus));

  t->editors.active = group == GTK_WIDGET(t->editors.groups[1]);
  update_status(t);
}

/* One of the two notebooks. Both share a group name, so tabs can be dragged between them. */
static GtkNotebook *new_group(App *t)
{
  GtkNotebook *group = GTK_NOTEBOOK(gtk_notebook_new());
  GtkEventController *focus = gtk_event_controller_focus_new();

  gtk_notebook_set_scrollable(group, TRUE);
  gtk_notebook_set_group_name(group, "t-code-editors");
  gtk_widget_set_hexpand(GTK_WIDGET(group), TRUE);
  g_signal_connect(group, "page-added", G_CALLBACK(on_tabs_changed), t);
  g_signal_connect(group, "page-removed", G_CALLBACK(on_tabs_changed), t);
  g_signal_connect(group, "switch-page", G_CALLBACK(on_tab_switched), t);
  g_signal_connect(focus, "enter", G_CALLBACK(on_group_focus), t);
  gtk_widget_add_controller(GTK_WIDGET(group), focus);
  return group;
}

/* The editor area: a hint while nothing is open, otherwise the two groups. */
void create_groups(App *t)
{
  GtkWidget *hint = gtk_label_new("Open a file from the file manager on the left, or press Ctrl+P");

  for (int g = 0; g < GROUPS; g++)
    t->editors.groups[g] = new_group(t);
  t->editors.split = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->editors.split, GTK_WIDGET(t->editors.groups[0]));
  gtk_paned_set_end_child(t->editors.split, GTK_WIDGET(t->editors.groups[1]));
  gtk_widget_set_visible(GTK_WIDGET(t->editors.groups[1]), FALSE);

  t->editors.area = GTK_STACK(gtk_stack_new());
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_stack_add_named(t->editors.area, hint, "empty");
  gtk_stack_add_named(t->editors.area, GTK_WIDGET(t->editors.split), "tabs");
  gtk_stack_set_visible_child_name(t->editors.area, "empty");
}
