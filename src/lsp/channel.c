/*
 * Language server messages over a pair of streams (see lspchannel.h): "Content-Length: <n>\r\n\r\n"
 * then n bytes of JSON, read and written asynchronously.
 */
#include "lsp/channel.h"

#include <stdlib.h>
#include <string.h>

#define MAX_HEADER 65536

/* Drops a reference; the last one closes the streams and frees the channel. */
static void channel_unref(Channel *ch)
{
  if (--ch->refs > 0)
    return;
  g_queue_clear_full(&ch->writes, (GDestroyNotify)g_bytes_unref);
  if (G_IS_IO_STREAM(ch->keep)) {
    g_io_stream_close(G_IO_STREAM(ch->keep), NULL, NULL);
  } else {
    g_output_stream_close(ch->out, NULL, NULL);
    g_input_stream_close(ch->in, NULL, NULL);
  }
  g_object_unref(ch->keep);
  g_object_unref(ch->cancel);
  g_byte_array_unref(ch->buf);
  g_free(ch);
}

void channel_close(Channel *ch)
{
  if (!ch->closed) {
    ch->closed = TRUE;
    g_cancellable_cancel(ch->cancel);
  }
  channel_unref(ch);
}

void channel_peer_closed(Channel *ch)
{
  if (ch->closed)
    return;
  ch->closed = TRUE;
  g_cancellable_cancel(ch->cancel);
  ch->on_closed(ch->owner);
}

/* The length from a message header ("Content-Length: 123\r\n..."); -1 without one. */
static long content_length(const char *header, gsize len)
{
  char *text = g_strndup(header, len);
  char **lines = g_strsplit(text, "\r\n", -1);
  long size = -1;

  for (int i = 0; lines[i]; i++)
    if (g_ascii_strncasecmp(lines[i], "Content-Length:", 15) == 0)
      size = strtol(lines[i] + 15, NULL, 10);
  g_strfreev(lines);
  g_free(text);
  return size;
}

/* Takes every whole message out of the buffer, calling the owner for each; a header that never
 * ends or has no length closes the channel. */
static void parse(Channel *ch)
{
  while (!ch->closed) {
    const char *data = (const char *)ch->buf->data;
    gsize len = ch->buf->len;
    const char *end = NULL;
    for (gsize i = 0; i + 4 <= len && !end; i++)
      if (memcmp(data + i, "\r\n\r\n", 4) == 0)
        end = data + i;
    if (!end) {
      if (len > MAX_HEADER)
        channel_peer_closed(ch);
      return;
    }
    gsize head = end - data + 4;
    long size = content_length(data, end - data);
    if (size < 0) {
      channel_peer_closed(ch);
      return;
    }
    if (len < head + (gsize)size)
      return;
    char *body = g_strndup(data + head, size);
    g_byte_array_remove_range(ch->buf, 0, head + size);
    ch->message(ch->owner, body);
    g_free(body);
  }
}

void channel_read(Channel *ch);

/* More bytes came: parse them and read on; nothing (EOF) or an error means the other side went. */
static void on_read(GObject *source, GAsyncResult *result, gpointer data)
{
  Channel *ch = data;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, NULL);

  if (!ch->closed && n > 0) {
    g_byte_array_append(ch->buf, (const guint8 *)ch->chunk, n);
    parse(ch);
  } else {
    channel_peer_closed(ch);
  }
  if (!ch->closed)
    channel_read(ch);
  channel_unref(ch);
}

void channel_read(Channel *ch)
{
  ch->refs++;
  g_input_stream_read_async(ch->in, ch->chunk, sizeof ch->chunk, G_PRIORITY_DEFAULT, ch->cancel, on_read, ch);
}

static void write_next(Channel *ch);

/* One message is written: the next one, if any waits. */
static void on_written(GObject *source, GAsyncResult *result, gpointer data)
{
  Channel *ch = data;
  gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);

  g_clear_pointer(&ch->writing, g_bytes_unref);
  if (!ok)
    channel_peer_closed(ch);
  else
    write_next(ch);
  channel_unref(ch);
}

/* Starts writing the next waiting message, unless one is being written. */
static void write_next(Channel *ch)
{
  gsize size;
  const void *data;

  if (ch->writing || ch->closed || g_queue_is_empty(&ch->writes))
    return;
  ch->writing = g_queue_pop_head(&ch->writes);
  data = g_bytes_get_data(ch->writing, &size);
  ch->refs++;
  g_output_stream_write_all_async(ch->out, data, size, G_PRIORITY_DEFAULT, ch->cancel, on_written, ch);
}

void channel_send(Channel *ch, const char *body)
{
  gsize len = strlen(body);
  GString *message;

  if (ch->closed)
    return;
  message = g_string_sized_new(len + 32);
  g_string_printf(message, "Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n", len);
  g_string_append_len(message, body, len);
  g_queue_push_tail(&ch->writes, g_string_free_to_bytes(message));
  write_next(ch);
}

Channel *channel_new(GInputStream *in, GOutputStream *out, GObject *keep, gpointer owner)
{
  Channel *ch = g_new0(Channel, 1);

  ch->refs = 1;
  ch->in = in;
  ch->out = out;
  ch->keep = g_object_ref(keep);
  ch->cancel = g_cancellable_new();
  ch->buf = g_byte_array_new();
  ch->owner = owner;
  g_queue_init(&ch->writes);
  return ch;
}
