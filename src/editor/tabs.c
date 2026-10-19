/*
 * The tabs and the two tab groups (see tabs.h). What an editor tab does is in editor.c; its page
 * is built in editorview.c.
 */
#include "editor/tabs.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/preview.h"
#include "git/compare.h"
#include "window/statusbar.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

/* The page went (its tab closed, or t-code quits): frees everything the tab holds. */
static void tab_free(Tab *tab)
{
  if (tab->preview)
    preview_free(tab->preview);
  editor_view_free(tab->view);
  g_free(tab->compare_with);
  g_free(tab->title);
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
  return gtk_notebook_get_n_pages(t->groups[0]) + gtk_notebook_get_n_pages(t->groups[1]);
}

Tab *tab_nth(App *t, int index)
{
  int first = gtk_notebook_get_n_pages(t->groups[0]);

  return index < first ? tab_of(gtk_notebook_get_nth_page(t->groups[0], index))
                       : tab_of(gtk_notebook_get_nth_page(t->groups[1], index - first));
}

int tab_group(Tab *tab)
{
  return gtk_notebook_page_num(tab->app->groups[1], tab->page) >= 0 ? 1 : 0;
}

Tab *current_tab(App *t)
{
  for (int k = 0; k < GROUPS; k++) {
    GtkNotebook *group = t->groups[k == 0 ? t->active : 1 - t->active];
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

  gtk_widget_grab_focus(tab ? focus_widget(tab) : GTK_WIDGET(t->fm));
}

void focus_group(App *t, int group)
{
  if (group == 1 && !gtk_widget_get_visible(GTK_WIDGET(t->groups[1])))
    return;
  t->active = group;
  focus_current_tab(t);
}

void select_tab(Tab *tab)
{
  App *t = tab->app;
  int group = tab_group(tab);

  gtk_notebook_set_current_page(t->groups[group], gtk_notebook_page_num(t->groups[group], tab->page));
  t->active = group;
  gtk_widget_grab_focus(focus_widget(tab));
}

void step_tab(App *t, int step)
{
  Tab *tab = current_tab(t);

  if (!tab)
    return;
  GtkNotebook *group = t->groups[tab_group(tab)];
  int n = gtk_notebook_get_n_pages(group);
  gtk_notebook_set_current_page(group, (gtk_notebook_get_current_page(group) + step + n) % n);
  focus_current_tab(t);
}

void remove_tab(Tab *tab)
{
  App *t = tab->app;
  GtkNotebook *group = t->groups[tab_group(tab)];

  if (tab->compare_with && !t->closing)
    forget_comparison(tab);
  unsync_scroll(tab);
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, tab->page));
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
    while (gtk_notebook_get_n_pages(t->groups[g]) > 0) {
      unsync_scroll(tab_of(gtk_notebook_get_nth_page(t->groups[g], 0)));
      gtk_notebook_remove_page(t->groups[g], 0);
    }
  t->closing = FALSE;
}

/* Labels ------------------------------------------------------------------------------------- */

void update_tab_label(Tab *tab)
{
  char *name = tab->title ? g_strdup(tab->title) : g_path_get_basename(tab->path);
  char *text = tab->kind == TAB_PREVIEW ? g_strdup_printf("Preview %s", name)
                                        : g_strdup_printf("%s%s", tab->modified ? "● " : "", name);

  gtk_label_set_text(tab->label, text);
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

/* A tab's label: its name (update_tab_label fills it) and a close button. */
static GtkWidget *tab_label(Tab *tab)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  GtkGesture *middle = gtk_gesture_click_new();

  tab->label = GTK_LABEL(gtk_label_new(NULL));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->label));
  gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
  gtk_widget_set_focusable(close, FALSE);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(close_tab), tab);
  gtk_box_append(GTK_BOX(box), close);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
  g_signal_connect(middle, "pressed", G_CALLBACK(on_tab_middle_click), tab);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(middle));
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
  GtkNotebook *group = t->groups[t->active];
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

/* Moves a tab (with its editor) to the end of the other group. */
void move_tab(Tab *tab, int to)
{
  App *t = tab->app;
  int from = tab_group(tab);
  GtkWidget *label = gtk_notebook_get_tab_label(t->groups[from], tab->page);

  if (from == to)
    return;
  g_object_ref(tab->page);
  g_object_ref(label);
  gtk_notebook_remove_page(t->groups[from], gtk_notebook_page_num(t->groups[from], tab->page));
  insert_page(t->groups[to], tab->page, label, -1);
  g_object_unref(label);
  g_object_unref(tab->page);
}

/* Groups ------------------------------------------------------------------------------------- */

void update_editor_area(App *t)
{
  gboolean split = gtk_widget_get_visible(GTK_WIDGET(t->groups[1]));

  gtk_stack_set_visible_child_name(t->editor_area, tab_count(t) > 0 || split ? "tabs" : "empty");
}

void set_split(App *t, gboolean split)
{
  GtkWidget *side = GTK_WIDGET(t->groups[1]);

  if (!split) {
    while (gtk_notebook_get_n_pages(t->groups[1]) > 0)
      move_tab(tab_of(gtk_notebook_get_nth_page(t->groups[1], 0)), 0);
    t->active = 0;
  }
  if (split != gtk_widget_get_visible(side)) {
    gtk_widget_set_visible(side, split);
    int width = gtk_widget_get_width(GTK_WIDGET(t->split));
    if (split && width > 0)
      gtk_paned_set_position(t->split, width / 2);
    if (split)
      t->active = 1;
  }
  if (t->split_button && gtk_toggle_button_get_active(t->split_button) != split)
    gtk_toggle_button_set_active(t->split_button, split);
  update_editor_area(t);
}

/* The second group goes away when its last tab closes. */
static gboolean unsplit_if_empty(gpointer data)
{
  App *t = data;

  if (!t->closing && gtk_widget_get_visible(GTK_WIDGET(t->groups[1])) && gtk_notebook_get_n_pages(t->groups[1]) == 0)
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
  if (group == t->groups[1] && gtk_notebook_get_n_pages(group) == 0)
    g_idle_add(unsplit_if_empty, t);
}

/* Another tab became the current one of a group: that group is the active one now. */
static void on_tab_switched(GtkNotebook *group, GtkWidget *page G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
  t->active = group == t->groups[1];
  update_status(t);
}

/* A click into a group makes it the active one (new tabs go there). */
static void on_group_focus(GtkEventControllerFocus *focus, App *t)
{
  GtkWidget *group = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(focus));

  t->active = group == GTK_WIDGET(t->groups[1]);
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
    t->groups[g] = new_group(t);
  t->split = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->split, GTK_WIDGET(t->groups[0]));
  gtk_paned_set_end_child(t->split, GTK_WIDGET(t->groups[1]));
  gtk_widget_set_visible(GTK_WIDGET(t->groups[1]), FALSE);

  t->editor_area = GTK_STACK(gtk_stack_new());
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_stack_add_named(t->editor_area, hint, "empty");
  gtk_stack_add_named(t->editor_area, GTK_WIDGET(t->split), "tabs");
  gtk_stack_set_visible_child_name(t->editor_area, "empty");
}
