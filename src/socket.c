/*
 * The unix socket the file manager talks to; its path is in $TCODE_SOCKET. One request per line:
 *   open <path>        open-side <path>    preview <path>     project <dir>
 *   compare <path>     history <path>      terminal <dir>     copy <text>
 * Paths are absolute. "compare" and "history" are git comparisons (gitcompare.c), "terminal"
 * does cd in the shell, "copy" puts text on the clipboard.
 */
#include "app.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <unistd.h>

static void cd_shell(App *t, const char *dir)
{
    char *quoted = g_shell_quote(dir);
    char *command = g_strdup_printf(" cd -- %s", quoted); /* the leading space keeps it out of history */

    run_in_shell(t, command);
    g_free(command);
    g_free(quoted);
}

static void handle(App *t, const char *command, const char *arg)
{
    if (strcmp(command, "copy") == 0) {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->window)), arg);
        return;
    }
    if (!g_path_is_absolute(arg))
        return;
    if (strcmp(command, "open") == 0)
        open_file(t, arg);
    else if (strcmp(command, "open-side") == 0)
        open_to_side(t, arg);
    else if (strcmp(command, "preview") == 0)
        open_preview(t, arg);
    else if (strcmp(command, "compare") == 0)
        compare_with_head(t, arg);
    else if (strcmp(command, "history") == 0)
        file_history(t, arg);
    else if (strcmp(command, "project") == 0 && g_file_test(arg, G_FILE_TEST_IS_DIR))
        open_project(t, arg);
    else if (strcmp(command, "terminal") == 0)
        cd_shell(t, arg);
}

static void on_line(GObject *source, GAsyncResult *result, gpointer data)
{
    GDataInputStream *in = G_DATA_INPUT_STREAM(source);
    App *t = data;
    char *line = g_data_input_stream_read_line_finish_utf8(in, result, NULL, NULL);

    if (!line) { /* closed, or not UTF-8 */
        g_object_unref(in);
        return;
    }
    char *space = strchr(line, ' ');
    if (space) {
        *space = '\0';
        handle(t, line, space + 1);
    }
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, on_line, t);
}

static gboolean on_incoming(GSocketService *service G_GNUC_UNUSED, GSocketConnection *conn,
                            GObject *source G_GNUC_UNUSED, App *t)
{
    GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));

    g_object_set_data_full(G_OBJECT(in), "connection", g_object_ref(conn), g_object_unref);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, on_line, t);
    return TRUE;
}

void start_socket(App *t)
{
    GSocketAddress *address;
    GError *error = NULL;

    t->socket_path = g_strdup_printf("%s/t-code-%d.sock", g_get_user_runtime_dir(), getpid());
    unlink(t->socket_path);
    address = g_unix_socket_address_new(t->socket_path);
    t->service = g_socket_service_new();
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(t->service), address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        g_warning("t-code: cannot listen on %s: %s", t->socket_path, error->message);
        g_clear_error(&error);
    }
    g_object_unref(address);
    g_signal_connect(t->service, "incoming", G_CALLBACK(on_incoming), t);
    g_socket_service_start(t->service);
}

void stop_socket(App *t)
{
    if (t->service)
        g_socket_service_stop(t->service);
    if (t->socket_path)
        unlink(t->socket_path);
}
