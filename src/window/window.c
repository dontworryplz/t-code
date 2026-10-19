/*
 * The main window: building it, its title bar and sidebar, and showing or hiding its panes (see
 * window.h for the layout).
 */
#include "window/window.h"

#include "dialogs/settings.h"
#include "editor/editorview.h"
#include "editor/preview.h"
#include "editor/tabs.h"
#include "git/scm.h"
#include "ipc/socket.h"
#include "popups/picker.h"
#include "project/project.h"
#include "project/session.h"
#include "search/search.h"
#include "window/commands.h"
#include "window/paths.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include "window/theme.h"

#include <string.h>

/* Panes -------------------------------------------------------------------------------------- */

void set_font_scale(App *t, double scale)
{
  int n = tab_count(t);

  t->font_scale = CLAMP(scale, 0.5, 3.0);
  vte_terminal_set_font_scale(t->fm, t->font_scale);
  vte_terminal_set_font_scale(t->shell, t->font_scale);
  editor_view_set_font(t);
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_PREVIEW)
      preview_set_zoom(tab_nth(t, i), t->font_scale);
}

void set_pane_visible(App *t, GtkWidget *pane, gboolean visible)
{
  if (!visible && gtk_widget_has_focus(pane))
    focus_current_tab(t);
  gtk_widget_set_visible(pane, visible);
}

void show_sidebar_page(App *t, const char *page)
{
  set_pane_visible(t, t->sidebar, TRUE);
  gtk_stack_set_visible_child_name(t->sidebar_stack, page);
  if (strcmp(page, "search") == 0)
    search_focus();
  else
    gtk_widget_grab_focus(strcmp(page, "files") == 0 ? GTK_WIDGET(t->fm) : t->scm);
}

void toggle_pane(App *t, GtkWidget *pane)
{
  gboolean show = !gtk_widget_get_visible(pane);

  set_pane_visible(t, pane, show);
  if (show)
    gtk_widget_grab_focus(pane);
}

/* Closing ------------------------------------------------------------------------------------ */

/* The window's close button, once nothing unsaved is left (or the user said to drop it): the
 * session is saved, then the window goes; its tabs stop their editors on the way. */
static void close_window(App *t, const char *arg G_GNUC_UNUSED)
{
  session_save(t);
  t->closing = TRUE;
  gtk_window_destroy(t->window);
}

/* The window manager asks to close: TRUE keeps the window until unless_unsaved() has asked about
 * unsaved changes. (Autosave leaves little: a second's worth of typing.) */
static gboolean on_close_request(GtkWindow *window G_GNUC_UNUSED, App *t)
{
  if (t->closing)
    return FALSE;
  unless_unsaved(t, "Close t-code?", "Close", close_window, NULL);
  return TRUE;
}

/* Title bar and sidebar ---------------------------------------------------------------------- */

/* The Split Editor button in the title bar. */
static void on_split_toggled(GtkToggleButton *button, App *t)
{
  set_split(t, gtk_toggle_button_get_active(button));
  focus_current_tab(t);
}

#define SIDEBAR_MIN_WIDTH 48 /* pixels: a few columns of the file manager */

/* Holds child without asking for its width: narrower, it is cut off on the right. */
static GtkWidget *clip_width(GtkWidget *child)
{
  GtkWidget *scrolled = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);
  return scrolled;
}

/* The sidebar goes down to a few columns of the file manager, which then draws itself that narrow. */
static void keep_sidebar_width(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  if (gtk_paned_get_position(paned) < SIDEBAR_MIN_WIDTH)
    gtk_paned_set_position(paned, SIDEBAR_MIN_WIDTH);
}

/* The left side: the file manager, the Source Control and the Search panels, as tabs. */
static GtkWidget *sidebar(App *t)
{
  static const char *const css =
      ".tcode-sidebar stackswitcher { padding: 5px; border-spacing: 4px; border-bottom: 1px solid #2b2b2b; }"
      ".tcode-sidebar stackswitcher button { min-height: 0; padding: 3px 10px; border-radius: 7px;"
      "  background: none; box-shadow: none; border: none; color: #9d9d9d; font-size: 0.8em; font-weight: bold; }"
      ".tcode-sidebar stackswitcher button:checked { color: #e7e7e7; background: rgba(255, 255, 255, .08); }"
      ".tcode-sidebar stackswitcher button:hover { color: #e7e7e7; }";
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *switcher = gtk_stack_switcher_new();

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  t->scm = clip_width(scm_panel(t));
  t->sidebar_stack = GTK_STACK(gtk_stack_new());
  gtk_stack_add_titled(t->sidebar_stack, GTK_WIDGET(t->fm), "files", "FILES");
  gtk_stack_add_titled(t->sidebar_stack, t->scm, "scm", "SOURCE CONTROL");
  gtk_stack_add_titled(t->sidebar_stack, clip_width(search_panel(t)), "search", "SEARCH");
  gtk_widget_set_vexpand(GTK_WIDGET(t->sidebar_stack), TRUE);
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), t->sidebar_stack);
  gtk_widget_set_focusable(switcher, FALSE);

  gtk_widget_add_css_class(box, "tcode-sidebar");
  make_card(box);
  gtk_box_append(GTK_BOX(box), clip_width(switcher));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(t->sidebar_stack));
  t->sidebar = box;
  return box;
}

/* Open Folder and Recent Folders on the left; the main menu, Settings and Split Editor on the right. */
static GtkWidget *header_bar(App *t)
{
  GtkWidget *header = gtk_header_bar_new();
  GtkWidget *open = gtk_button_new_from_icon_name("folder-open-symbolic");
  GtkWidget *recent = gtk_menu_button_new();
  GtkWidget *split = gtk_toggle_button_new();
  GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
  GtkWidget *menu = gtk_menu_button_new();
  GMenu *items = g_menu_new();

  gtk_widget_set_tooltip_text(open, "Open Folder… (Ctrl+Shift+O)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open-folder");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);

  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(recent), "document-open-recent-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(recent), G_MENU_MODEL(t->recent_menu));
  gtk_widget_set_tooltip_text(recent, "Recent Folders");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), recent);

  gtk_button_set_icon_name(GTK_BUTTON(split), "view-dual-symbolic");
  gtk_widget_set_tooltip_text(split, "Split Editor (Ctrl+\\)");
  t->split_button = GTK_TOGGLE_BUTTON(split);
  g_signal_connect(split, "toggled", G_CALLBACK(on_split_toggled), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), split);

  gtk_widget_set_tooltip_text(settings, "Settings (Ctrl+,)");
  g_signal_connect_swapped(settings, "clicked", G_CALLBACK(show_settings), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), settings);

  g_menu_append(items, "Keyboard Shortcuts", "win.keybinds");
  g_menu_append(items, "About t-code", "win.about");
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu), G_MENU_MODEL(items));
  gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu), TRUE);
  gtk_widget_set_tooltip_text(menu, "Main Menu");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu);
  g_object_unref(items);
  return header;
}

/* The window --------------------------------------------------------------------------------- */

void build_window(App *t, GtkApplication *app)
{
  GtkWidget *overlay = gtk_overlay_new();                        /* the picker (Ctrl+P) floats over the content */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); /* the panes, the status bar */

  /* Before the window: the theme, where the helpers are, and the sockets they connect to. */
  load_theme();
  gtk_source_init();
  find_files(t);
  start_socket(t);
  gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), t->icon_dir);
  gtk_window_set_default_icon_name("io.github.tcode");

  t->window = GTK_WINDOW(gtk_application_window_new(app));
  gtk_window_set_default_size(t->window, 1400, 900);
  add_project_actions(t);
  gtk_window_set_titlebar(t->window, header_bar(t));
  g_signal_connect(t->window, "close-request", G_CALLBACK(on_close_request), t);
  update_title(t);

  create_fixed_terminals(t);
  create_groups(t);

  /* The editors above the shell. */
  make_card(GTK_WIDGET(t->editor_area));
  /* The terminal paints no background of its own: the card's rounded one shows instead. */
  gtk_widget_add_css_class(GTK_WIDGET(t->shell), "tcode-shell");
  make_card(GTK_WIDGET(t->shell));
  vte_terminal_set_clear_background(t->shell, FALSE);
  t->vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
  gtk_paned_set_start_child(t->vpaned, GTK_WIDGET(t->editor_area));
  gtk_paned_set_end_child(t->vpaned, GTK_WIDGET(t->shell));
  gtk_paned_set_resize_end_child(t->vpaned, FALSE);
  gtk_paned_set_position(t->vpaned, 620);

  /* The sidebar left of them. */
  t->hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->hpaned, sidebar(t));
  gtk_paned_set_end_child(t->hpaned, GTK_WIDGET(t->vpaned));
  gtk_paned_set_resize_start_child(t->hpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->hpaned, FALSE);
  g_signal_connect(t->hpaned, "notify::position", G_CALLBACK(keep_sidebar_width), NULL);
  gtk_paned_set_position(t->hpaned, 280);
  gtk_widget_set_vexpand(GTK_WIDGET(t->hpaned), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(t->hpaned), "tcode-workspace");

  gtk_box_append(GTK_BOX(content), GTK_WIDGET(t->hpaned));
  gtk_box_append(GTK_BOX(content), status_bar(t));
  gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
  picker_init(t, GTK_OVERLAY(overlay));
  gtk_window_set_child(t->window, overlay);

  /* The last session's sizes, then start the programs and show it. */
  add_shortcuts(t);
  session_apply_window(t);
  if (strcmp(t->root, g_get_home_dir()) != 0) /* home means no folder (see reopen_last_project) */
    session_add_recent(t);
  rebuild_recent_menu(t);
  start_fixed_terminals(t);
  start_status_updates(t);
  gtk_window_present(t->window);
  gtk_widget_grab_focus(GTK_WIDGET(t->fm));
}
