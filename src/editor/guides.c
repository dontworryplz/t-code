/*
 * Indent guides (see guides.h): TcodeView, a GtkSourceView whose layer below the text (in buffer
 * coordinates, so it scrolls with it) gets the guides of the lines on the screen. The block around
 * the cursor comes from its brackets, found outside strings and comments; it is worked out again
 * only when the text or the cursor changed.
 */
#include "editor/guides.h"

#include "editor/editorview.h"

#include <string.h>

struct _TcodeView {
  GtkSourceView parent;
  Tab *tab;
  struct {
    guint changes;
    int cursor;      /* offset */
    int open, close; /* lines of the brackets around the cursor, -1 for none */
  } pair;            /* the last pair worked out */
};

#define TCODE_TYPE_VIEW (tcode_view_get_type())
G_DECLARE_FINAL_TYPE(TcodeView, tcode_view, TCODE, VIEW, GtkSourceView)
G_DEFINE_FINAL_TYPE(TcodeView, tcode_view, GTK_SOURCE_TYPE_VIEW)

/* Dark Modern: editorIndentGuide.background1, .activeBackground1 (without code coloring) and
 * editorBracketHighlight.foreground1..3, the bracket pair colors by depth. */
static const GdkRGBA guide_ink = { 0.25, 0.25, 0.25, 1 }, active_ink = { 0.44, 0.44, 0.44, 1 };
static const GdkRGBA pair_inks[3] = { { 1.0, 0.84, 0.0, 1 }, { 0.85, 0.44, 0.84, 1 }, { 0.09, 0.62, 1.0, 1 } };

/* Brackets ----------------------------------------------------------------------------------- */

typedef struct {
  int line;
  char c;
} Bracket;

/* The brackets of text outside strings and comments, in order, with their lines. */
static GArray *brackets_of(const char *text)
{
  GArray *found = g_array_new(FALSE, FALSE, sizeof(Bracket));
  gboolean comment = FALSE;
  int line = 0;

  for (const char *p = text; *p; p++) {
    if (*p == '\n') {
      line++;
    } else if (comment) {
      if (p[0] == '*' && p[1] == '/') {
        comment = FALSE;
        p++;
      }
    } else if (p[0] == '/' && p[1] == '/') {
      while (p[1] && p[1] != '\n')
        p++;
    } else if (p[0] == '/' && p[1] == '*') {
      comment = TRUE;
      p++;
    } else if (*p == '"' || *p == '\'') {
      char quote = *p;
      for (p++; *p && *p != quote && *p != '\n'; p++)
        if (*p == '\\' && p[1])
          p++;
      if (!*p)
        break;
      if (*p == '\n')
        line++;
    } else if (strchr("([{)]}", *p)) {
      Bracket b = { line, *p };
      g_array_append_val(found, b);
    }
  }
  return found;
}

static gboolean is_opening(char c)
{
  return c == '(' || c == '[' || c == '{';
}

/* The innermost bracket pair around offset with lines between its two lines: their lines in *open
 * and *close; FALSE when there is none. */
static gboolean enclosing_pair(GtkTextBuffer *buffer, int offset, int *open, int *close)
{
  GtkTextIter start, cursor, end;
  char *before, *after;
  GArray *head, *tail, *stack = g_array_new(FALSE, FALSE, sizeof(int));
  int cursor_line, depth = 0;
  gboolean found = FALSE;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_get_iter_at_offset(buffer, &cursor, offset);
  cursor_line = gtk_text_iter_get_line(&cursor);
  before = gtk_text_buffer_get_text(buffer, &start, &cursor, TRUE);
  after = gtk_text_buffer_get_text(buffer, &cursor, &end, TRUE);
  head = brackets_of(before);
  tail = brackets_of(after); /* lines from the cursor's line on */

  /* Before the cursor, the stack ends up with the lines of the brackets still open. */
  for (guint i = 0; i < head->len; i++) {
    Bracket *b = &g_array_index(head, Bracket, i);
    if (is_opening(b->c))
      g_array_append_val(stack, b->line);
    else if (stack->len > 0)
      g_array_set_size(stack, stack->len - 1);
  }
  /* After it, a closing bracket at depth 0 closes the innermost one still open. */
  for (guint i = 0; i < tail->len && stack->len > 0 && !found; i++) {
    Bracket *b = &g_array_index(tail, Bracket, i);
    if (is_opening(b->c)) {
      depth++;
    } else if (depth > 0) {
      depth--;
    } else {
      *open = g_array_index(stack, int, stack->len - 1);
      *close = cursor_line + b->line;
      g_array_set_size(stack, stack->len - 1);
      found = *close - *open >= 2;
    }
  }
  g_array_free(stack, TRUE);
  g_array_free(tail, TRUE);
  g_array_free(head, TRUE);
  g_free(after);
  g_free(before);
  return found;
}

/* Drawing ------------------------------------------------------------------------------------ */

/* Columns of leading blanks of a line (tabs to the next stop), -1 when it is blank. */
static int indent_of(GtkTextBuffer *buffer, int line, int tab_size)
{
  GtkTextIter iter;
  int column = 0;

  if (line < 0 || line >= gtk_text_buffer_get_line_count(buffer))
    return -1;
  gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
  for (; !gtk_text_iter_ends_line(&iter); gtk_text_iter_forward_char(&iter)) {
    gunichar c = gtk_text_iter_get_char(&iter);
    if (c == '\t')
      column += tab_size - column % tab_size;
    else if (c == ' ')
      column++;
    else
      return column;
  }
  return -1;
}

/* Guides on a blank line follow the lines around it, like VS Code's. */
static int guide_count(GtkTextBuffer *buffer, int line, int tab_size)
{
  int indent = indent_of(buffer, line, tab_size), above = -1, below = -1;

  if (indent >= 0)
    return (indent + tab_size - 1) / tab_size;
  for (int i = line - 1; i >= 0 && i > line - 200 && above < 0; i--)
    above = indent_of(buffer, i, tab_size);
  for (int i = line + 1, n = gtk_text_buffer_get_line_count(buffer); i < n && i < line + 200 && below < 0; i++)
    below = indent_of(buffer, i, tab_size);
  if (above < 0 && below < 0)
    return 0;
  if (above < 0 || below < 0 || above == below)
    return (MAX(above, below) + tab_size - 1) / tab_size;
  return 1 + MIN(above, below) / tab_size; /* inside the block one of them opens or closes */
}

/* The block around the cursor, again only when the text or the cursor changed. */
static gboolean cursor_pair(TcodeView *self, int *open, int *close)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(self->tab->buffer);
  GtkTextIter cursor;
  int offset;

  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  offset = gtk_text_iter_get_offset(&cursor);
  if (self->pair.changes != self->tab->view->changes || self->pair.cursor != offset || self->pair.open == -2) {
    self->pair.changes = self->tab->view->changes;
    self->pair.cursor = offset;
    if (!enclosing_pair(buffer, offset, &self->pair.open, &self->pair.close))
      self->pair.open = self->pair.close = -1;
  }
  *open = self->pair.open;
  *close = self->pair.close;
  return *open >= 0;
}

static void draw_guide(GtkSnapshot *snapshot, const GdkRGBA *ink, double x, int y, int height)
{
  gtk_snapshot_append_color(snapshot, ink, &GRAPHENE_RECT_INIT((float)x, (float)y, 1, (float)height));
}

static void draw_guides(TcodeView *self, GtkSnapshot *snapshot)
{
  GtkTextView *view = GTK_TEXT_VIEW(self);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(self->tab->buffer);
  int ts = MAX((int)gtk_source_view_get_tab_width(GTK_SOURCE_VIEW(self)), 1), first, last, open, close;
  int width = 0;
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "0000000000");
  GdkRectangle visible, start;
  GtkTextIter iter;
  double cw, left;

  pango_layout_get_pixel_size(layout, &width, NULL);
  g_object_unref(layout);
  cw = width / 10.0;
  gtk_text_view_get_visible_rect(view, &visible);
  gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
  first = gtk_text_iter_get_line(&iter);
  gtk_text_view_get_iter_location(view, &iter, &start); /* a line's start: the text's left edge */
  left = start.x;
  gtk_text_view_get_line_at_y(view, &iter, visible.y + visible.height, NULL);
  last = gtk_text_iter_get_line(&iter);

  for (int line = first; line <= last; line++) {
    int y, height, count = guide_count(buffer, line, ts);
    gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
    gtk_text_view_get_line_yrange(view, &iter, &y, &height);
    for (int k = 0; k < count; k++)
      draw_guide(snapshot, &guide_ink, left + k * ts * cw, y, height);
  }
  if (cursor_pair(self, &open, &close)) {
    int column = MIN(indent_of(buffer, open, ts) < 0 ? G_MAXINT : indent_of(buffer, open, ts),
                     indent_of(buffer, close, ts) < 0 ? G_MAXINT : indent_of(buffer, close, ts));
    const GdkRGBA *ink = self->tab->app->plain ? &active_ink : &pair_inks[column / ts % 3];
    for (int line = MAX(open + 1, first); column != G_MAXINT && line < close && line <= last; line++) {
      int y, height, indent = indent_of(buffer, line, ts);
      if (indent >= 0 && indent <= column)
        continue;
      gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
      gtk_text_view_get_line_yrange(view, &iter, &y, &height);
      draw_guide(snapshot, ink, left + column * cw, y, height);
    }
  }
}

static void snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot)
{
  TcodeView *self = TCODE_VIEW(view);

  GTK_TEXT_VIEW_CLASS(tcode_view_parent_class)->snapshot_layer(view, layer, snapshot);
  if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT && self->tab && self->tab->view && !self->tab->view->loading &&
      self->tab->app->settings.guides)
    draw_guides(self, snapshot);
}

static void tcode_view_class_init(TcodeViewClass *klass)
{
  GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = snapshot_layer;
}

static void tcode_view_init(TcodeView *self)
{
  self->pair.open = -2; /* not worked out yet */
}

/* The cursor moved: the block around it may be another one. */
static void on_mark_set(GtkTextBuffer *buffer, const GtkTextIter *where G_GNUC_UNUSED, GtkTextMark *mark,
                        TcodeView *self)
{
  if (mark == gtk_text_buffer_get_insert(buffer) && self->tab->app->settings.guides)
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

GtkSourceView *guides_view_new(Tab *tab, GtkSourceBuffer *buffer)
{
  TcodeView *self = g_object_new(TCODE_TYPE_VIEW, "buffer", buffer, NULL);

  self->tab = tab;
  g_signal_connect_object(buffer, "mark-set", G_CALLBACK(on_mark_set), self, 0);
  return GTK_SOURCE_VIEW(self);
}
