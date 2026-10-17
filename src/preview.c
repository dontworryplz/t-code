/*
 * Markdown preview (Ctrl+Shift+V, or "Open Preview" in the file manager), styled like VS Code's.
 * md4c turns the file into HTML (GitHub flavor: tables, task lists, strikethrough, autolinks) and
 * WebKit shows it. The preview follows the file as it is saved, keeping its scroll position.
 */
#include "app.h"

#include <md4c-html.h>
#include <string.h>
#include <webkit/webkit.h>

struct Preview {
    Tab *tab;
    WebKitWebView *view;
    GFileMonitor *monitor;
    guint reload_id;
    gboolean loaded;
};

static const char *const css =
    ":root { color-scheme: dark; }"
    "html, body { background: #1f1f1f; color: #cccccc; }"
    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe WPC', 'Segoe UI', system-ui, 'Ubuntu',"
    "  'Droid Sans', sans-serif; font-size: 14px; line-height: 1.6; max-width: 882px; margin: 0 auto;"
    "  padding: 0 26px 32px; word-wrap: break-word; }"
    "#content > :first-child { margin-top: 16px; }"
    "h1, h2, h3, h4, h5, h6 { font-weight: 600; line-height: 1.25; margin: 24px 0 16px; }"
    "h1 { font-size: 2em; } h2 { font-size: 1.5em; } h3 { font-size: 1.25em; } h6 { color: #9d9d9d; }"
    "h1, h2 { padding-bottom: .3em; border-bottom: 1px solid rgba(255, 255, 255, .18); }"
    "p, ul, ol, blockquote, table, pre { margin: 0 0 16px; }"
    "a { color: #4daafc; text-decoration: none; } a:hover { text-decoration: underline; }"
    "code { font-family: 'Cascadia Code', monospace; font-size: .9em; color: #d0d0d0;"
    "  background: #3c3c3c; padding: 1px 4px; border-radius: 4px; }"
    "pre { background: #2b2b2b; padding: 16px; border-radius: 4px; overflow: auto; line-height: 1.45; }"
    "pre code { background: none; padding: 0; color: #cccccc; }"
    "blockquote { margin-left: 0; padding: 0 16px; border-left: 5px solid #616161;"
    "  background: rgba(127, 127, 127, .1); }"
    "blockquote > p { padding: 8px 0; margin: 0; }"
    "table { border-collapse: collapse; }"
    "th, td { border: 1px solid rgba(255, 255, 255, .18); padding: 5px 10px; }"
    "th { text-align: left; font-weight: 600; }"
    "hr { border: 0; height: 1px; background: rgba(255, 255, 255, .18); margin: 24px 0; }"
    "img { max-width: 100%; }"
    "li.task-list-item { list-style: none; } li.task-list-item input { margin: 0 .4em 0 -1.3em; }"
    ".error { color: #f14c4c; }";

gboolean is_markdown(const char *path)
{
    static const char *const extensions[] = { ".md", ".markdown", ".mdown", ".mkd", NULL };
    const char *ext = strrchr(path, '.');

    for (int i = 0; ext && extensions[i]; i++)
        if (g_ascii_strcasecmp(ext, extensions[i]) == 0)
            return TRUE;
    return FALSE;
}

/* Rendering ---------------------------------------------------------------------------------- */

static void append_html(const MD_CHAR *text, MD_SIZE size, void *out)
{
    g_string_append_len(out, text, size);
}

static char *render_body(const char *path)
{
    char *text = NULL;
    gsize len = 0;
    GError *error = NULL;

    if (!g_file_get_contents(path, &text, &len, &error)) {
        char *message = g_markup_escape_text(error->message, -1);
        char *html = g_strdup_printf("<p class=\"error\">%s</p>", message);
        g_free(message);
        g_error_free(error);
        return html;
    }
    GString *out = g_string_new(NULL);
    md_html(text, (MD_SIZE)len, append_html, out, MD_DIALECT_GITHUB, 0);
    g_free(text);
    return g_string_free(out, FALSE);
}

static char *js_string(const char *s)
{
    GString *out = g_string_new("\"");

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '\\': g_string_append(out, "\\\\"); break;
        case '"': g_string_append(out, "\\\""); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if (*p < 0x20)
                g_string_append_printf(out, "\\u%04x", *p);
            else
                g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

static void reload(Preview *p)
{
    char *body = render_body(p->tab->path);

    if (p->loaded) {
        /* Swap the content in place, so the scroll position stays. */
        char *literal = js_string(body);
        char *script = g_strdup_printf("document.getElementById('content').innerHTML = %s;", literal);
        webkit_web_view_evaluate_javascript(p->view, script, -1, NULL, NULL, NULL, NULL, NULL);
        g_free(script);
        g_free(literal);
    } else {
        char *base = g_filename_to_uri(p->tab->path, NULL, NULL);
        char *page = g_strdup_printf("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>%s</style>"
                                     "</head><body><div id=\"content\">%s</div></body></html>",
                                     css, body);
        webkit_web_view_load_html(p->view, page, base);
        g_free(page);
        g_free(base);
    }
    g_free(body);
}

static gboolean reload_now(gpointer data)
{
    Preview *p = data;

    p->reload_id = 0;
    reload(p);
    return G_SOURCE_REMOVE;
}

static void on_file_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED,
                            GFile *other G_GNUC_UNUSED, GFileMonitorEvent event, Preview *p)
{
    if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
        return;
    if (p->reload_id)
        g_source_remove(p->reload_id);
    p->reload_id = g_timeout_add(150, reload_now, p);
}

static void on_load_changed(WebKitWebView *view G_GNUC_UNUSED, WebKitLoadEvent event, Preview *p)
{
    if (event == WEBKIT_LOAD_FINISHED)
        p->loaded = TRUE;
}

/* Links: anchors scroll inside the page, markdown files open as previews, other local files in
 * an editor, and everything else in the default app (the browser, for web links). */
static gboolean on_decide_policy(WebKitWebView *view G_GNUC_UNUSED, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, Preview *p)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;

    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    if (webkit_navigation_action_get_navigation_type(action) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
        return FALSE;

    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    char *base = g_filename_to_uri(p->tab->path, NULL, NULL);
    const char *hash = strchr(uri, '#');
    char *target = hash ? g_strndup(uri, hash - uri) : g_strdup(uri);

    if (g_strcmp0(target, base) == 0 && type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        g_free(target);
        g_free(base);
        return FALSE; /* an anchor in this document */
    }
    webkit_policy_decision_ignore(decision);

    char *path = g_str_has_prefix(target, "file://") ? g_filename_from_uri(target, NULL, NULL) : NULL;
    if (path && is_markdown(path) && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        open_preview(p->tab->app, path);
    } else if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        open_file(p->tab->app, path);
    } else {
        GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
        gtk_uri_launcher_launch(launcher, p->tab->app->window, NULL, NULL, NULL);
        g_object_unref(launcher);
    }
    g_free(path);
    g_free(target);
    g_free(base);
    return TRUE;
}

/* Tabs --------------------------------------------------------------------------------------- */

void preview_free(Preview *p)
{
    if (p->reload_id)
        g_source_remove(p->reload_id);
    if (p->monitor) {
        g_file_monitor_cancel(p->monitor);
        g_object_unref(p->monitor);
    }
    g_free(p);
}

/* WebKit's 100% looks small next to the 11pt terminals, so previews start zoomed in. */
#define PREVIEW_ZOOM 1.25

void preview_set_zoom(Tab *tab, double scale)
{
    if (tab->preview)
        webkit_web_view_set_zoom_level(tab->preview->view, scale * PREVIEW_ZOOM);
}

/* One network session for all previews, held only by their web views: WebKit's network process
 * (~60 MB) quits when the last preview closes. The default session kept it until t-code quit. */
static WebKitNetworkSession *preview_session(void)
{
    static WebKitNetworkSession *session;

    if (session)
        return g_object_ref(session);
    session = webkit_network_session_new_ephemeral();
    g_object_add_weak_pointer(G_OBJECT(session), (gpointer *)&session);
    return session;
}

void open_preview(App *t, const char *path)
{
    int n = tab_count(t);

    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_PREVIEW && g_strcmp0(tab->path, path) == 0) {
            select_tab(tab);
            return;
        }
    }

    Preview *p = g_new0(Preview, 1);
    GdkRGBA background;
    GFile *file = g_file_new_for_path(path);
    WebKitNetworkSession *session = preview_session();

    p->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, NULL));
    g_object_unref(session);
    gdk_rgba_parse(&background, "#1f1f1f");
    webkit_web_view_set_background_color(p->view, &background);
    /* Scripts inside the markdown never run; only the content swap above does. */
    webkit_settings_set_enable_javascript_markup(webkit_web_view_get_settings(p->view), FALSE);
    webkit_web_view_set_zoom_level(p->view, t->font_scale * PREVIEW_ZOOM);
    gtk_widget_set_hexpand(GTK_WIDGET(p->view), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(p->view), TRUE);
    g_signal_connect(p->view, "load-changed", G_CALLBACK(on_load_changed), p);
    g_signal_connect(p->view, "decide-policy", G_CALLBACK(on_decide_policy), p);

    p->tab = add_tab(t, TAB_PREVIEW, GTK_WIDGET(p->view), path);
    p->tab->preview = p;
    p->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (p->monitor)
        g_signal_connect(p->monitor, "changed", G_CALLBACK(on_file_changed), p);
    g_object_unref(file);

    reload(p);
    focus_current_tab(t);
}
