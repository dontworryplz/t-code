/*
 * Language server messages over a pair of streams, the way LSP frames them:
 * "Content-Length: <n>\r\n\r\n" then n bytes of JSON. Reads and writes are asynchronous; writes
 * wait their turn in a queue. Used for both sides of ipc/lsp.c: the editors' connections and the
 * servers' stdin / stdout.
 */
#pragma once

#include <gio/gio.h>

typedef struct Channel Channel;

struct Channel {
    int refs;                /* the owner's, and one per read or write in flight */
    GInputStream *in;
    GOutputStream *out;
    GObject *keep;           /* the connection or process the streams belong to */
    GCancellable *cancel;
    GByteArray *buf;         /* read, not yet a whole message */
    char chunk[65536];
    GQueue writes;           /* GBytes waiting their turn */
    GBytes *writing;
    gboolean closed;
    gboolean header;         /* a line comes before the messages (clients) */
    void (*line)(gpointer owner, const char *line);
    void (*message)(gpointer owner, const char *body);
    void (*on_closed)(gpointer owner); /* the other side went: the owner calls channel_close() */
    gpointer owner;
};

/* A channel over in and out, which belong to keep (a connection or a process, held until the
 * channel goes). header: a line comes before the messages (editors send which server they want).
 * The owner sets line, message and on_closed, then calls channel_read(). */
Channel *channel_new(GInputStream *in, GOutputStream *out, GObject *keep, gboolean header, gpointer owner);

/* Starts reading; the owner's callbacks get what comes. */
void channel_read(Channel *ch);

/* Sends one message (body is its JSON). */
void channel_send(Channel *ch, const char *body);

/* By the owner: drops its reference; no on_closed. */
void channel_close(Channel *ch);

/* The other side went (or sent garbage): stops the channel and tells the owner (on_closed). */
void channel_peer_closed(Channel *ch);
