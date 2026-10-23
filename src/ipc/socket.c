/*
 * The control socket, for scripts and tools (tools/headless.sh, packaging/smoke-test.sh); its path
 * is in $TCODE_SOCKET. One request per line; a client that ends its requests with NUL bytes instead
 * (a path may hold a line break: printf 'open %s\0' "$path") ends them all that way:
 *   open <path>        open-side <path>    open-window <path> preview <path>     project <dir>
 *   compare <path>     history <path>      terminal <dir>     copy <text>
 *   debug <path>       debug-failed <dir>  say <text>         insert <text>      append <text>
 *   replace-all <text> get-file            get-project        get-text           save subscribe
 * Paths are absolute. "compare" and "history" are git comparisons (git/compare.c), "terminal"
 * does cd in the shell, "copy" puts text on the clipboard, "debug" starts the debugger on a program
 * that was just built for it (t-code --send, from project/run.c; a folder: pick one in it), "debug-failed"
 * says that build failed.
 */
#include "ipc/socket.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "editor/preview.h"
#include "git/compare.h"
#include "project/project.h"
#include "editor/tabs.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

static gboolean event_client_add(App *t, GOutputStream *out)
{
  if (!t->socket_events)
    t->socket_events = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < t->socket_events->len; i++)
    if (g_ptr_array_index(t->socket_events, i) == out)
      return FALSE;
  g_ptr_array_add(t->socket_events, g_object_ref(out));
  return TRUE;
}

static void event_client_remove(App *t, GOutputStream *out)
{
  for (guint i = 0; t->socket_events && i < t->socket_events->len; i++)
    if (g_ptr_array_index(t->socket_events, i) == out) {
      g_ptr_array_remove_index(t->socket_events, i);
      return;
    }
}

static void broadcast_event(App *t, const char *event, const char *path)
{
  JsonNode *node = json_node_init_string(json_node_alloc(), path);
  char *quoted = json_to_string(node, FALSE);
  char *message = g_strdup_printf("{\"event\":\"%s\",\"path\":%s}\n", event, quoted);
  gsize length = strlen(message);

  json_node_unref(node);
  g_free(quoted);
  for (guint i = 0; t->socket_events && i < t->socket_events->len;) {
    GOutputStream *out = g_ptr_array_index(t->socket_events, i);
    if (!g_output_stream_write_all(out, message, length, NULL, NULL, NULL) ||
        !g_output_stream_flush(out, NULL, NULL))
      g_ptr_array_remove_index(t->socket_events, i);
    else
      i++;
  }
  g_free(message);
}

void socket_did_open(App *t, const char *path)
{
  broadcast_event(t, "did_open", path);
}


static void subscribe_events(App *t, GOutputStream *out)
{
  static const char response[] = "{\"event\":\"subscribed\"}\n";

  event_client_add(t, out);
  if (!g_output_stream_write_all(out, response, sizeof response - 1, NULL, NULL, NULL))
    event_client_remove(t, out);
}

void socket_did_save(App *t, const char *path)
{
  broadcast_event(t, "did_save", path);
}


/* One request (see the top of this file). */
static void handle(App *t, const char *command, const char *arg, GOutputStream *out)
{
  if (strcmp(command, "subscribe") == 0) {
    subscribe_events(t, out);
    return;
  }
  if (strcmp(command, "say") == 0) {
    status_say(t, arg);
    return;
  }
  if (strcmp(command, "insert") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->buffer)
      gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), arg, -1);
    return;
  }
  if (strcmp(command, "append") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->buffer) {
      GtkTextIter end;
      gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(tab->buffer), &end);
      gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &end, arg, -1);
    }
    return;
  }
  if (strcmp(command, "replace-all") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->buffer)
      gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), arg, -1);
    return;
  }
  if (strcmp(command, "get-file") == 0) {
    if (out) {
      Tab *tab = current_tab(t);
      const char *path = (tab && tab->kind == TAB_EDITOR && tab->path) ? tab->path : "";
      g_output_stream_write_all(out, path, strlen(path), NULL, NULL, NULL);
      g_output_stream_write_all(out, "\n", 1, NULL, NULL, NULL);
      g_output_stream_flush(out, NULL, NULL);
    }
    return;
  }
  if (strcmp(command, "get-project") == 0) {
    if (out) {
      const char *root = t->project.root ? t->project.root : "";
      g_output_stream_write_all(out, root, strlen(root), NULL, NULL, NULL);
      g_output_stream_write_all(out, "\n", 1, NULL, NULL, NULL);
      g_output_stream_flush(out, NULL, NULL);
    }
    return;
  }
  if (strcmp(command, "get-text") == 0) {
    if (out) {
      Tab *tab = current_tab(t);
      if (tab && tab->kind == TAB_EDITOR && tab->buffer) {
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
        char *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
        if (text) {
          g_output_stream_write_all(out, text, strlen(text), NULL, NULL, NULL);
          g_free(text);
        }
      }
      g_output_stream_write_all(out, "\0", 1, NULL, NULL, NULL);
      g_output_stream_flush(out, NULL, NULL);
    }
    return;
  }
  if (strcmp(command, "save") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR)
      save_editor(tab);
    return;
  }
  if (strcmp(command, "copy") == 0) {
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->window)), arg);
    return;
  }
  if (strcmp(command, "debug-failed") == 0) {
    debug_build_failed(t);
    return;
  }
  if (!g_path_is_absolute(arg))
    return;
  if (strcmp(command, "open") == 0)
    open_file(t, arg);
  else if (strcmp(command, "open-side") == 0)
    open_to_side(t, arg);
  else if (strcmp(command, "open-window") == 0)
    open_in_new_window(t, arg);
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
  else if (strcmp(command, "debug") == 0)
    debug_launch(t, arg);
}

static void read_request(GDataInputStream *in, App *t);

/* A request came on a connection: handle it and read the next one; the connection ends at EOF. */
static void on_request(GObject *source, GAsyncResult *result, gpointer data)
{
  GDataInputStream *in = G_DATA_INPUT_STREAM(source);
  App *t = data;
  GSocketConnection *conn = g_object_get_data(G_OBJECT(in), "connection");
  GOutputStream *out = conn ? g_io_stream_get_output_stream(G_IO_STREAM(conn)) : NULL;
  gsize length = 0;
  char *request = g_data_input_stream_read_upto_finish(in, result, &length, NULL);
  GError *error = NULL;

  /* The end it stopped at is still there; at EOF there is none. */
  g_data_input_stream_read_byte(in, NULL, &error);
  if (!request || (error && length == 0)) {
    event_client_remove(t, out);
    g_clear_error(&error);
    g_free(request);
    g_object_unref(in);
    return;
  }
  g_clear_error(&error);

  if (g_utf8_validate(request, length, NULL)) {
    char *space = strchr(request, ' ');
    if (space) {
      *space = '\0';
      handle(t, request, space + 1, out);
    } else {
      handle(t, request, "", out);
    }
  }
  g_free(request);
  read_request(in, t);
}

/* Data came: a connection that sends a NUL byte ends its requests with NULs only (a path may hold a
 * line break), else with line breaks. */
static void on_filled(GObject *source, GAsyncResult *result, gpointer data)
{
  GDataInputStream *in = G_DATA_INPUT_STREAM(source);
  App *t = data;
  gssize got = g_buffered_input_stream_fill_finish(G_BUFFERED_INPUT_STREAM(in), result, NULL);
  gsize size = 0;
  const char *buffer = g_buffered_input_stream_peek_buffer(G_BUFFERED_INPUT_STREAM(in), &size);

  if (got <= 0 && size == 0) {
    GSocketConnection *conn = g_object_get_data(G_OBJECT(in), "connection");
    GOutputStream *out = conn ? g_io_stream_get_output_stream(G_IO_STREAM(conn)) : NULL;
    event_client_remove(t, out);
    g_object_unref(in);
    return;
  }
  if (memchr(buffer, '\0', size))
    g_object_set_data(G_OBJECT(in), "nul", GINT_TO_POINTER(TRUE));
  read_request(in, t);
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
  if (!t->socket_events)
    t->socket_events = g_ptr_array_new_with_free_func(g_object_unref);
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
  g_clear_pointer(&t->socket_events, g_ptr_array_unref);
}
