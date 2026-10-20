/*
 * The Settings window (Ctrl+, or the gear in the title bar). Every change applies at once, to the
 * open editors too, and is saved in settings.ini, which can also be edited by hand.
 */
#include "dialogs/settings.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/client.h"
#include "project/session.h"
#include "project/userfiles.h"
#include "window/terminal.h"
#include "window/theme.h"
#include "window/window.h"

static const char *const css = ".tcode-settings { background: #1f1f1f; }"
                               ".tcode-settings .section { font-weight: bold; color: #e7e7e7; margin: 18px 0 6px; }"
                               ".tcode-settings .name { color: #cccccc; }"
                               ".tcode-settings .note { color: #9d9d9d; font-size: smaller; }";

static GtkWidget *window; /* the Settings window while it is open */

/* Applies the settings again to every open editor. */
static void each_editor(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_EDITOR)
      editor_view_apply_settings(tab_nth(t, i));
}

/* The App, from any widget in the Settings window (show_settings hangs it on the window). */
static App *app_of(GtkWidget *widget)
{
  return g_object_get_data(G_OBJECT(gtk_widget_get_root(widget)), "t-code-app");
}

/* The switches and spin buttons: each sets its setting, applies it to the open tabs and saves. */
static void on_minimap(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.minimap = gtk_switch_get_active(toggle);
  each_editor(t);
  session_save_settings(t);
}

static void on_sticky_scroll(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.sticky_scroll = gtk_switch_get_active(toggle);
  each_editor(t);
  session_save_settings(t);
}

static void on_coloring(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  if (gtk_switch_get_active(toggle) == t->plain)
    toggle_syntax(t); /* saves the settings */
}

static void on_tabs_to_spaces(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.tabs_to_spaces = gtk_switch_get_active(toggle);
  each_editor(t);
  session_save_settings(t);
}

static void on_tab_size(GtkSpinButton *spin, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(spin));

  t->settings.tab_size = gtk_spin_button_get_value_as_int(spin);
  each_editor(t);
  session_save_settings(t);
}

static void on_detect_indent(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.detect_indent = gtk_switch_get_active(toggle);
  session_save_settings(t);
}

static void on_guides(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.guides = gtk_switch_get_active(toggle);
  each_editor(t);
  session_save_settings(t);
}

static void on_word_wrap(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.word_wrap = gtk_switch_get_active(toggle);
  each_editor(t);
  session_save_settings(t);
}

/* clangd starts again with the checks on or off. */
static void on_clang_tidy(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.clang_tidy = gtk_switch_get_active(toggle);
  lsp_restart(t);
  session_save_settings(t);
}

static void on_format_on_save(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.format_on_save = gtk_switch_get_active(toggle);
  session_save_settings(t);
}

static void on_autosave(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.autosave = gtk_switch_get_active(toggle);
  session_save_settings(t);
}

static void on_theme(GtkDropDown *choice, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(choice));

  t->settings.square = gtk_drop_down_get_selected(choice) == 1;
  apply_theme(t);
  session_save_settings(t);
}

/* The "Open" buttons: data opens a file to edit by hand, in an editor tab over this window. */
static void on_open_file(GtkButton *button, gpointer data)
{
  App *t = app_of(GTK_WIDGET(button));
  void (*open)(App *t) = data;

  gtk_window_close(GTK_WINDOW(window));
  open(t);
}

static GtkWidget *open_button(void (*open)(App *t))
{
  GtkWidget *button = gtk_button_new_with_label("Open");

  g_signal_connect(button, "clicked", G_CALLBACK(on_open_file), open);
  return button;
}

static void on_reopen_last(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  t->settings.reopen_last = gtk_switch_get_active(toggle);
  session_save_settings(t);
}

static void on_text_size(GtkSpinButton *spin, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(spin));

  set_font_scale(t, gtk_spin_button_get_value(spin) / 100);
  session_save_settings(t); /* kept even if t-code does not close normally */
}

/* Font buttons: data is the setting (&t->settings.font or files_font) they change. */
static void on_font(GtkFontDialogButton *button, GParamSpec *spec G_GNUC_UNUSED, gpointer data)
{
  App *t = app_of(GTK_WIDGET(button));
  PangoFontDescription *desc = gtk_font_dialog_button_get_font_desc(button);
  char **setting = data;

  if (!desc)
    return;
  g_free(*setting);
  *setting = pango_font_description_to_string(desc);
  apply_fonts(t);
  session_save_settings(t);
}

/* The font dialog's filter: fixed width font families (and their faces) only. */
static gboolean is_monospace(gpointer item, gpointer data G_GNUC_UNUSED)
{
  PangoFontFamily *family = PANGO_IS_FONT_FACE(item) ? pango_font_face_get_family(item) : item;

  return PANGO_IS_FONT_FAMILY(family) && pango_font_family_is_monospace(family);
}

/* A font button for a font setting. Only fixed width fonts are listed: code is aligned by columns,
 * and a terminal puts every character in a cell of the same width. */
static GtkWidget *font_for(char **setting)
{
  GtkFontDialog *dialog = gtk_font_dialog_new();
  GtkWidget *button = gtk_font_dialog_button_new(dialog); /* takes the dialog */
  PangoFontDescription *desc = pango_font_description_from_string(*setting);
  GtkCustomFilter *filter = gtk_custom_filter_new(is_monospace, NULL, NULL);

  gtk_font_dialog_set_title(dialog, "Font");
  gtk_font_dialog_set_filter(dialog, GTK_FILTER(filter));
  g_object_unref(filter);
  gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button), desc);
  pango_font_description_free(desc);
  g_signal_connect(button, "notify::font-desc", G_CALLBACK(on_font), setting);
  return button;
}

/* A section heading ("Editor", "Window") across both columns. */
static void add_heading(GtkGrid *grid, int *row, const char *title)
{
  GtkWidget *heading = gtk_label_new(title);

  gtk_widget_add_css_class(heading, "section");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_grid_attach(grid, heading, 0, (*row)++, 2, 1);
}

/* A row: the name (with a note under it when given) and the control on the right. */
static void add_row(GtkGrid *grid, int *row, const char *name, const char *note, GtkWidget *control)
{
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *label = gtk_label_new(name);

  gtk_widget_add_css_class(label, "name");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_box_append(GTK_BOX(labels), label);
  if (note) {
    GtkWidget *small = gtk_label_new(note);
    gtk_widget_add_css_class(small, "note");
    gtk_label_set_xalign(GTK_LABEL(small), 0);
    gtk_label_set_wrap(GTK_LABEL(small), TRUE);
    gtk_box_append(GTK_BOX(labels), small);
  }
  gtk_widget_set_hexpand(labels, TRUE);
  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(control, GTK_ALIGN_END);
  gtk_grid_attach(grid, labels, 0, *row, 1, 1);
  gtk_grid_attach(grid, control, 1, (*row)++, 1, 1);
}

/* A switch, set to on, that calls changed when it is flipped. */
static GtkWidget *switch_for(gboolean on, GCallback changed)
{
  GtkWidget *toggle = gtk_switch_new();

  gtk_switch_set_active(GTK_SWITCH(toggle), on);
  g_signal_connect(toggle, "notify::active", changed, NULL);
  return toggle;
}

/* A number field from low to high, set to value, that calls changed when it changes. */
static GtkWidget *spin_for(double low, double high, double step, double value, GCallback changed)
{
  GtkWidget *spin = gtk_spin_button_new_with_range(low, high, step);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  g_signal_connect(spin, "value-changed", changed, NULL);
  return spin;
}

/* Escape closes the window. */
static gboolean on_key(GtkEventControllerKey *controller, guint key, guint code G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  if (key != GDK_KEY_Escape)
    return FALSE;
  gtk_window_close(GTK_WINDOW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))));
  return TRUE;
}

void settings_changed(App *t, gboolean restart_lsp)
{
  if (window) /* its switches show the old values */
    gtk_window_destroy(GTK_WINDOW(window));
  each_editor(t);
  apply_fonts(t);
  apply_theme(t);
  if (restart_lsp)
    lsp_restart(t);
}

void show_settings(App *t)
{
  static gboolean styled;
  GtkCssProvider *provider;
  GtkWidget *grid;
  GtkEventController *keys;
  int row = 0;

  if (window) {
    gtk_window_present(GTK_WINDOW(window));
    return;
  }
  if (!styled) {
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    styled = TRUE;
  }

  window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), "Settings");
  gtk_window_set_transient_for(GTK_WINDOW(window), t->window);
  gtk_window_set_default_size(GTK_WINDOW(window), 560, 640); /* smaller than the settings: they scroll */
  gtk_widget_add_css_class(window, "tcode-settings");
  g_object_set_data(G_OBJECT(window), "t-code-app", t);
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);

  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  gtk_widget_set_margin_start(grid, 24);
  gtk_widget_set_margin_end(grid, 24);
  gtk_widget_set_margin_bottom(grid, 24);

  add_heading(GTK_GRID(grid), &row, "Editor");
  add_row(GTK_GRID(grid), &row, "Minimap", "A small picture of the file next to the scrollbar",
          switch_for(t->settings.minimap, G_CALLBACK(on_minimap)));
  add_row(GTK_GRID(grid), &row, "Sticky scroll",
          "The first lines of the blocks around the top (namespace, function, loop) stay at the top",
          switch_for(t->settings.sticky_scroll, G_CALLBACK(on_sticky_scroll)));
  add_row(GTK_GRID(grid), &row, "Code coloring", "Also F7", switch_for(!t->plain, G_CALLBACK(on_coloring)));
  add_row(GTK_GRID(grid), &row, "Indent guides",
          "Lines at each indentation level; the block around the cursor in color",
          switch_for(t->settings.guides, G_CALLBACK(on_guides)));
  add_row(GTK_GRID(grid), &row, "Word wrap", "Lines longer than the editor continue on the next row",
          switch_for(t->settings.word_wrap, G_CALLBACK(on_word_wrap)));
  add_row(GTK_GRID(grid), &row, "Tab size",
          "For projects without their own: the status bar's choice, .editorconfig or .clang-format come first",
          spin_for(1, 16, 1, t->settings.tab_size, G_CALLBACK(on_tab_size)));
  add_row(GTK_GRID(grid), &row, "Insert spaces for Tab", NULL,
          switch_for(t->settings.tabs_to_spaces, G_CALLBACK(on_tabs_to_spaces)));
  add_row(GTK_GRID(grid), &row, "Detect indentation",
          "Files indented with tabs or another number of spaces keep it (applies to files opened from now on)",
          switch_for(t->settings.detect_indent, G_CALLBACK(on_detect_indent)));
  add_row(GTK_GRID(grid), &row, "Autosave", "Save a second after the last change",
          switch_for(t->settings.autosave, G_CALLBACK(on_autosave)));
  add_row(GTK_GRID(grid), &row, "Format on save",
          "Ctrl+S formats C and C++ files with clangd first (the project's .clang-format); autosave does not",
          switch_for(t->settings.format_on_save, G_CALLBACK(on_format_on_save)));
  add_row(GTK_GRID(grid), &row, "clang-tidy checks",
          "clangd also reports what the project's .clang-tidy asks for (more CPU while typing)",
          switch_for(t->settings.clang_tidy, G_CALLBACK(on_clang_tidy)));

  add_heading(GTK_GRID(grid), &row, "Window");
  GtkWidget *theme = gtk_drop_down_new_from_strings((const char *const[]){ "Rounded", "Square", NULL });
  gtk_drop_down_set_selected(GTK_DROP_DOWN(theme), t->settings.square ? 1 : 0);
  g_signal_connect(theme, "notify::selected", G_CALLBACK(on_theme), NULL);
  add_row(GTK_GRID(grid), &row, "Theme", "Rounded panels on a darker frame, or flat ones with lines between", theme);
  add_row(GTK_GRID(grid), &row, "Editor font", "Editors and the terminal", font_for(&t->settings.font));
  add_row(GTK_GRID(grid), &row, "Files panel font", "File icons need a Nerd Font installed",
          font_for(&t->settings.files_font));
  add_row(GTK_GRID(grid), &row, "Text size (%)", "Also Ctrl+= and Ctrl+-",
          spin_for(50, 300, 10, t->font_scale * 100, G_CALLBACK(on_text_size)));
  add_row(GTK_GRID(grid), &row, "Reopen the last project",
          "When started from the app menu (not from a folder in a terminal)",
          switch_for(t->settings.reopen_last, G_CALLBACK(on_reopen_last)));

  add_heading(GTK_GRID(grid), &row, "Edit by hand");
  add_row(GTK_GRID(grid), &row, "Settings file", "settings.ini: these settings, each with a note",
          open_button(open_settings_file));
  add_row(GTK_GRID(grid), &row, "Theme CSS", "theme.css: your own CSS over t-code's look (colors, corners, sizes)",
          open_button(open_user_theme));
  add_row(GTK_GRID(grid), &row, "Editor colors", "Your copy of the editor's color scheme (dark-modern.xml)",
          open_button(open_editor_colors));

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
  gtk_window_set_child(GTK_WINDOW(window), scroll);
  keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
  gtk_widget_add_controller(window, keys);
  gtk_window_present(GTK_WINDOW(window));
}
