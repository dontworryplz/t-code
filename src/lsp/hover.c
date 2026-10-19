/*
 * The hover (see hover.h): a GtkSourceHoverProvider. GtkSourceView shows its popover after the
 * pointer rests for a moment; the provider fills it, asking the server (textDocument/hover) on the way.
 */
#include "lsp/hover.h"

#include "lsp/client.h"
#include "lsp/diagnostics.h"

#define MAX_WIDTH_CHARS 90

struct _TcodeHover {
  GObject parent;
  Tab *tab;
};

#define TCODE_TYPE_HOVER (tcode_hover_get_type())
G_DECLARE_FINAL_TYPE(TcodeHover, tcode_hover, TCODE, HOVER, GObject)

static void hover_provider_init(GtkSourceHoverProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeHover, tcode_hover, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_HOVER_PROVIDER, hover_provider_init))

static const char *const hover_css =
    ".tcode-hover { padding: 4px 6px; }"
    ".tcode-hover label.symbol { font-family: 'Cascadia Code', monospace; font-size: 0.95em; color: #cccccc; }"
    ".tcode-hover separator { margin: 6px 0; background: #454545; }";

/* One hover being filled. */
typedef struct {
  GtkSourceHoverDisplay *display;
  char *problems; /* Pango markup, or NULL */
} Job;

static void job_free(Job *job)
{
  g_object_unref(job->display);
  g_free(job->problems);
  g_free(job);
}

/* A label of at most MAX_WIDTH_CHARS columns, wrapped. */
static GtkWidget *hover_label(const char *text, gboolean markup, const char *css_class)
{
  GtkWidget *label = gtk_label_new(NULL);

  if (markup)
    gtk_label_set_markup(GTK_LABEL(label), text);
  else
    gtk_label_set_text(GTK_LABEL(label), text);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), MAX_WIDTH_CHARS);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_selectable(GTK_LABEL(label), FALSE);
  if (css_class)
    gtk_widget_add_css_class(label, css_class);
  return label;
}

/* The server's text: a MarkupContent ({"value": ...}), a string, or a list of them (old servers). */
static char *hover_text(JsonNode *result)
{
  JsonNode *contents = json_at(result, "contents");
  const char *value = json_string_at(contents, "value");

  if (!value && contents && JSON_NODE_HOLDS_VALUE(contents))
    value = json_node_get_string(contents);
  if (!value)
    value = json_string_at(contents, "0.value");
  if (!value)
    value = json_string_at(contents, "0");
  if (!value || !*value)
    return NULL;
  /* clangd's plain text has a blank line between every part: one line break is enough here. */
  GRegex *blank_lines = g_regex_new("\n\\s*\n", 0, 0, NULL);
  char *text = g_regex_replace_literal(blank_lines, value, -1, 0, "\n", 0, NULL);
  g_regex_unref(blank_lines);
  return g_strstrip(text);
}

/* Fills the popover with the problems and the server's text; with neither, nothing shows. */
static void fill(GTask *task, JsonNode *result)
{
  Job *job = g_task_get_task_data(task);
  char *text = result ? hover_text(result) : NULL;
  GtkWidget *box;

  if (!job->problems && !text) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Nothing to show");
    return;
  }
  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "tcode-hover");
  if (job->problems)
    gtk_box_append(GTK_BOX(box), hover_label(job->problems, TRUE, NULL));
  if (job->problems && text)
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  if (text)
    gtk_box_append(GTK_BOX(box), hover_label(text, FALSE, "symbol"));
  gtk_source_hover_display_append(job->display, box);
  g_task_return_boolean(task, TRUE);
  g_free(text);
}

static void on_hover(JsonNode *result, gpointer data)
{
  GTask *task = data;

  fill(task, result);
  g_object_unref(task);
}

static void hover_populate_async(GtkSourceHoverProvider *provider, GtkSourceHoverContext *context,
                                 GtkSourceHoverDisplay *display, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer data)
{
  Tab *tab = TCODE_HOVER(provider)->tab;
  GTask *task = g_task_new(provider, cancellable, callback, data);
  Job *job = g_new0(Job, 1);
  GtkTextIter iter;

  job->display = g_object_ref(display);
  g_task_set_task_data(task, job, (GDestroyNotify)job_free);
  if (!gtk_source_hover_context_get_iter(context, &iter)) {
    fill(task, NULL);
    g_object_unref(task);
    return;
  }
  job->problems = diagnostics_at(tab, &iter, TRUE);
  if (lsp_has(tab)) {
    char *position = lsp_position_params(tab, &iter), *params = g_strdup_printf("{%s}", position);
    lsp_request(tab, "textDocument/hover", params, cancellable, on_hover, task);
    g_free(params);
    g_free(position);
  } else {
    on_hover(NULL, task);
  }
}

static gboolean hover_populate_finish(GtkSourceHoverProvider *provider G_GNUC_UNUSED, GAsyncResult *result,
                                      GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void hover_provider_init(GtkSourceHoverProviderInterface *iface)
{
  iface->populate_async = hover_populate_async;
  iface->populate_finish = hover_populate_finish;
}

static void tcode_hover_class_init(TcodeHoverClass *klass G_GNUC_UNUSED)
{
}

static void tcode_hover_init(TcodeHover *self G_GNUC_UNUSED)
{
}

void hover_attach(Tab *tab)
{
  static gboolean styled;
  TcodeHover *provider = g_object_new(TCODE_TYPE_HOVER, NULL);
  GtkSourceHover *hover = gtk_source_view_get_hover(tab->source);

  if (!styled) {
    GtkCssProvider *style = gtk_css_provider_new();
    gtk_css_provider_load_from_string(style, hover_css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(style),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
    styled = TRUE;
  }
  provider->tab = tab;
  gtk_source_hover_add_provider(hover, GTK_SOURCE_HOVER_PROVIDER(provider));
  g_object_unref(provider); /* the hover holds it */
}
