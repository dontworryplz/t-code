/*
 * An editor tab's page (see editorview.h): the source view, the minimap next to it, the style
 * scheme, the font and the settings.
 */
#include "editor/editorview.h"

#include "editor/detect.h"
#include "editor/diffgutter.h"
#include "editor/editing.h"
#include "editor/findbar.h"
#include "editor/guides.h"
#include "lsp/client.h"
#include "lsp/completion.h"
#include "lsp/diagnostics.h"
#include "lsp/hover.h"
#include "lsp/navigate.h"
#include "lsp/semantic.h"
#include "lsp/signature.h"
#include "window/terminal.h"

#define SCHEME "t-code-dark-modern" /* styles/dark-modern.xml */

static const char *const css =
    ".tcode-editor scrollbar { background: none; border: none; }"
    ".tcode-editor scrollbar slider { min-width: 6px; border-radius: 6px; background: rgba(121, 121, 121, .4); }"
    ".tcode-editor scrollbar slider:hover { background: rgba(100, 100, 100, .7); }"
    ".tcode-editor textview, .tcode-editor textview text { background: #1f1f1f; }"
    ".tcode-editor textview border.left gutter { background: #1f1f1f; padding-right: 12px; }"
    ".tcode-editor .tcode-minimap { border-left: 1px solid #2b2b2b; }"
    ".tcode-editor .tcode-overview { border-left: 1px solid #2b2b2b; }";

#define OVERVIEW_WIDTH 8 /* pixels, like VS Code's overview ruler */

static GtkCssProvider *font_provider; /* the editor font; editor_view_set_font() rewrites it */

/* The first editor: the CSS all editors share, and where the style scheme is. */
static void init_look(App *t)
{
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  font_provider = gtk_css_provider_new();
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(font_provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  editor_view_set_font(t);
  if (t->styles_dir)
    gtk_source_style_scheme_manager_prepend_search_path(schemes, t->styles_dir);
}

void editor_view_set_font(App *t)
{
  PangoFontDescription *desc = pango_font_description_from_string(t->settings.font ? t->settings.font : DEFAULT_FONT);
  double size = pango_font_description_get_size(desc) / (double)PANGO_SCALE;
  char points[G_ASCII_DTOSTR_BUF_SIZE], *rule;

  if (!font_provider)
    return;
  if (size <= 0)
    size = 11;
  /* "11.5", also where the locale writes "11,5" (CSS would reject it) */
  g_ascii_formatd(points, sizeof points, "%.1f", size * t->font_scale);
  rule = g_strdup_printf(
      ".tcode-editor textview.tcode-source { font-family: \"%s\"; font-size: %spt;"
      " font-feature-settings: \"calt\" 0, \"liga\" 0; }", /* "->" stays two characters */
      pango_font_description_get_family(desc) ? pango_font_description_get_family(desc) : "monospace", points);
  gtk_css_provider_load_from_string(font_provider, rule);
  g_free(rule);
  pango_font_description_free(desc);
}

GtkSourceLanguage *editor_language(const char *path, const char *text)
{
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
  char *type = g_content_type_guess(path, NULL, 0, NULL);
  GtkSourceLanguage *language = gtk_source_language_manager_guess_language(languages, path, type);

  if (language && g_strcmp0(gtk_source_language_get_id(language), "chdr") == 0 && text && is_cxx_text(text, -1))
    language = gtk_source_language_manager_get_language(languages, "cpphdr");
  g_free(type);
  return language;
}

void editor_view_reveal_cursor(Tab *tab)
{
  GtkTextView *view = GTK_TEXT_VIEW(tab->source);
  GtkTextMark *insert = gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer));
  GtkAdjustment *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
  GdkRectangle visible, cursor;
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &iter, insert);
  gtk_text_view_get_visible_rect(view, &visible);
  gtk_text_view_get_iter_location(view, &iter, &cursor);
  if (vertical && (cursor.y < visible.y || cursor.y + cursor.height > visible.y + visible.height))
    gtk_adjustment_set_value(vertical, cursor.y + cursor.height / 2.0 - gtk_adjustment_get_page_size(vertical) / 2);
  /* Far lines have estimated heights until laid out: this makes it exact. */
  gtk_text_view_scroll_mark_onscreen(view, insert);
}

void editor_view_apply_settings(Tab *tab)
{
  App *t = tab->app;
  EditorView *v = tab->view;
  GtkSourceView *view = tab->source;
  int width = v->own_indent ? v->own_width : t->settings.tab_size;
  gboolean spaces = v->own_indent ? !v->own_tabs : t->settings.tabs_to_spaces;

  gtk_source_view_set_tab_width(view, MAX(width, 1));
  gtk_source_view_set_indent_width(view, -1); /* the tab width */
  gtk_source_view_set_insert_spaces_instead_of_tabs(view, spaces);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), t->settings.word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
  gtk_source_buffer_set_highlight_syntax(tab->buffer, !t->plain);
  gtk_widget_set_visible(v->map, t->settings.minimap);
  if (lsp_has(tab))
    semantic_update(tab);                  /* code coloring may have been turned off or on */
  gtk_widget_queue_draw(GTK_WIDGET(view)); /* the indent guides, turned off or on */
}

/* The problems strip ------------------------------------------------------------------------- */

/* A mark per line with problems, where that line is in the whole file; errors over warnings. */
static void draw_overview(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width, int height, gpointer data)
{
  Tab *tab = data;
  GArray *lines[2] = { g_array_new(FALSE, FALSE, sizeof(int)), g_array_new(FALSE, FALSE, sizeof(int)) };
  double total = MAX(gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(tab->buffer)), 1);
  static const double rgb[2][3] = { { 0.80, 0.65, 0.0 }, { 0.95, 0.30, 0.30 } }; /* warning, error */

  diagnostics_lines(tab, lines[1], lines[0]);
  for (int k = 0; k < 2; k++) {
    cairo_set_source_rgb(cr, rgb[k][0], rgb[k][1], rgb[k][2]);
    for (guint i = 0; i < lines[k]->len; i++) {
      double y = g_array_index(lines[k], int, i) / total * height;
      cairo_rectangle(cr, 1, MIN(y, height - 3.0), width - 1, 3);
    }
    cairo_fill(cr);
    g_array_free(lines[k], TRUE);
  }
}

/* A click on the strip goes to that part of the file. */
static void on_overview_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, Tab *tab)
{
  GtkWidget *strip = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  int line = (int)(y / MAX(gtk_widget_get_height(strip), 1) * gtk_text_buffer_get_line_count(buffer));
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
  gtk_text_buffer_place_cursor(buffer, &iter);
  editor_view_reveal_cursor(tab);
  gtk_widget_grab_focus(GTK_WIDGET(tab->source));
}

void editor_view_show_problems(Tab *tab)
{
  gtk_widget_queue_draw(tab->view->overview);
}

/* The page ----------------------------------------------------------------------------------- */

GtkWidget *editor_page(void)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class(page, "tcode-editor");
  return page;
}

void editor_view_init(Tab *tab, gboolean readonly)
{
  static gboolean styled;
  App *t = tab->app;
  EditorView *v = g_new0(EditorView, 1);
  GtkWidget *page = tab->page;
  GtkWidget *scrolled = gtk_scrolled_window_new();
  GtkSourceBuffer *buffer;
  GtkSourceView *view;

  if (!styled) {
    init_look(t);
    styled = TRUE;
  }
  buffer = gtk_source_buffer_new(NULL);
  gtk_source_buffer_set_style_scheme(
      buffer, gtk_source_style_scheme_manager_get_scheme(gtk_source_style_scheme_manager_get_default(), SCHEME));
  gtk_source_buffer_set_highlight_matching_brackets(buffer, TRUE);
  view = guides_view_new(tab, buffer); /* our reference to the buffer stays the tab's */

  tab->view = v;
  tab->source = view;
  tab->buffer = buffer;
  v->tab = tab;
  v->readonly = readonly;
  v->file = gtk_source_file_new();
  v->cancel = g_cancellable_new();
  v->next_waiters = g_array_new(FALSE, FALSE, sizeof(SaveWaiter));
  editing_attach(tab);
  diff_gutter_attach(tab);
  hover_attach(tab);
  completion_attach(tab);
  navigate_attach(tab);

  gtk_widget_add_css_class(GTK_WIDGET(view), "tcode-source");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), !readonly);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 200); /* the last line can come up from the bottom */
  gtk_source_view_set_show_line_numbers(view, TRUE);
  gtk_source_view_set_highlight_current_line(view, TRUE);
  gtk_source_view_set_auto_indent(view, TRUE);
  gtk_source_view_set_indent_on_tab(view, TRUE);
  gtk_source_view_set_smart_backspace(view, TRUE);
  gtk_source_view_set_smart_home_end(view, GTK_SOURCE_SMART_HOME_END_BEFORE);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(view));
  v->overlay = gtk_overlay_new(); /* the find bar goes over the text */
  gtk_overlay_set_child(GTK_OVERLAY(v->overlay), scrolled);
  gtk_widget_set_hexpand(v->overlay, TRUE);
  gtk_widget_set_vexpand(v->overlay, TRUE);
  gtk_box_append(GTK_BOX(page), v->overlay);

  v->map = gtk_source_map_new();
  gtk_source_map_set_view(GTK_SOURCE_MAP(v->map), view);
  gtk_widget_add_css_class(v->map, "tcode-minimap");
  gtk_box_append(GTK_BOX(page), v->map);

  GtkGesture *click = gtk_gesture_click_new();
  v->overview = gtk_drawing_area_new();
  gtk_widget_set_size_request(v->overview, OVERVIEW_WIDTH, -1);
  gtk_widget_add_css_class(v->overview, "tcode-overview");
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->overview), draw_overview, tab, NULL);
  g_signal_connect(click, "pressed", G_CALLBACK(on_overview_click), tab);
  gtk_widget_add_controller(v->overview, GTK_EVENT_CONTROLLER(click));
  gtk_box_append(GTK_BOX(page), v->overview);
  signature_attach(tab); /* in the overlay */
  editor_view_apply_settings(tab);
}

void editor_view_free(EditorView *v)
{
  if (!v)
    return;
  g_cancellable_cancel(v->cancel); /* loads and saves in flight drop their answer */
  lsp_close(v->tab);
  diagnostics_clear(v->tab);
  signature_free(v->signature);
  semantic_free(v->semantic);
  findbar_free(v->find);
  diff_gutter_free(v->diff);
  g_free(v->closers);
  for (guint i = 0; i < v->next_waiters->len; i++) {
    SaveWaiter *w = &g_array_index(v->next_waiters, SaveWaiter, i);
    w->func(w->data, FALSE);
  }
  g_array_free(v->next_waiters, TRUE);
  if (v->monitor) {
    g_file_monitor_cancel(v->monitor);
    g_object_unref(v->monitor);
  }
  if (v->autosave_id)
    g_source_remove(v->autosave_id);
  if (v->reload_id)
    g_source_remove(v->reload_id);
  if (v->scroll_id)
    g_source_remove(v->scroll_id);
  g_signal_handlers_disconnect_by_data(v->tab->buffer, v->tab);
  g_object_unref(v->tab->buffer); /* the page went first: the tab still holds it */
  g_object_unref(v->cancel);
  g_object_unref(v->file);
  g_free(v->etag);
  g_free(v);
}
