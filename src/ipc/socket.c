/*
 * The control socket, for scripts and tools (tools/headless.sh, packaging/smoke-test.sh); its path
 * is in $TCODE_SOCKET. One request per line; a client that ends its requests with NUL bytes instead
 * (a path may hold a line break: printf 'open %s\0' "$path") ends them all that way:
 *   open <path>        open-side <path>    preview <path>     project <dir>
 *   compare <path>     history <path>      terminal <dir>     copy <text>
 * Paths are absolute. "compare" and "history" are git comparisons (git/compare.c), "terminal"
 * does cd in the shell, "copy" puts text on the clipboard.
 */
#include "ipc/socket.h"

#include "editor/editor.h"
#include "editor/preview.h"
#include "git/compare.h"
#include "project/project.h"
#include "window/terminal.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <unistd.h>

/* One request (see the top of this file). */
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
    cd_in_shell(t, arg);
}

static void read_request(GDataInputStream *in, App *t);

/* A request came on a connection: handle it and read the next one; the connection ends at EOF. */
static void on_request(GObject *source, GAsyncResult *result, gpointer data)
{
  GDataInputStream *in = G_DATA_INPUT_STREAM(source);
  App *t = data;
  gsize length = 0;
  char *request = g_data_input_stream_read_upto_finish(in, result, &length, NULL);
  GError *error = NULL;

  /* The end it stopped at is still there; at EOF there is none. */
  g_data_input_stream_read_byte(in, NULL, &error);
  if (!request || (error && length == 0)) { /* closed */
    g_clear_error(&error);
    g_free(request);
    g_object_unref(in);
    return;
  }
  g_clear_error(&error);
  char *space = strchr(request, ' ');
  if (space && g_utf8_validate(request, length, NULL)) {
    *space = '\0';
    handle(t, request, space + 1);
  }
  g_free(request);
  read_request(in, t);
}

/* Data came: a connection that sends a NUL byte ends its requests with NULs only (a path may hold a
 * line break), else with line breaks. */
static void on_filled(GObject *source, GAsyncResult *result, gpointer data)
{
  GDataInputStream *in = G_DATA_INPUT_STREAM(source);
  gssize got = g_buffered_input_stream_fill_finish(G_BUFFERED_INPUT_STREAM(in), result, NULL);
  gsize size = 0;
  const char *buffer = g_buffered_input_stream_peek_buffer(G_BUFFERED_INPUT_STREAM(in), &size);

  if (got <= 0 && size == 0) { /* closed */
    g_object_unref(in);
    return;
  }
  if (memchr(buffer, '\0', size))
    g_object_set_data(G_OBJECT(in), "nul", GINT_TO_POINTER(TRUE));
  read_request(in, data);
}

static void read_request(GDataInputStream *in, App *t)
{
  gsize size = 0;

  if (g_object_get_data(G_OBJECT(in), "nul"))
    g_data_input_stream_read_upto_async(in, "", 1, G_PRIORITY_DEFAULT, NULL, on_request, t); /* up to a NUL */
  else if (g_buffered_input_stream_peek_buffer(G_BUFFERED_INPUT_STREAM(in), &size) && size > 0)
    g_data_input_stream_read_upto_async(in, "\n", 1, G_PRIORITY_DEFAULT, NULL, on_request, t);
  else
    g_buffered_input_stream_fill_async(G_BUFFERED_INPUT_STREAM(in), -1, G_PRIORITY_DEFAULT, NULL, on_filled, t);
}

/* A client connected; its lines are read until it closes the connection. */
static gboolean on_incoming(GSocketService *service G_GNUC_UNUSED, GSocketConnection *conn,
                            GObject *source G_GNUC_UNUSED, App *t)
{
  GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));

  g_object_set_data_full(G_OBJECT(in), "connection", g_object_ref(conn), g_object_unref);
  read_request(in, t);
  return TRUE;
}

void start_socket(App *t)
{
  GSocketAddress *address;
  GError *error = NULL;

  t->paths.socket = g_strdup_printf("%s/t-code-%d.sock", g_get_user_runtime_dir(), getpid());
  unlink(t->paths.socket);
  address = g_unix_socket_address_new(t->paths.socket);
  t->service = g_socket_service_new();
  if (!g_socket_listener_add_address(G_SOCKET_LISTENER(t->service), address, G_SOCKET_TYPE_STREAM,
                                     G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
    g_warning("t-code: cannot listen on %s: %s", t->paths.socket, error->message);
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
  if (t->paths.socket)
    unlink(t->paths.socket);
}
