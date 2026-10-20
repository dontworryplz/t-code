/*
 * The selection's other occurrences (see occurrences.h).
 */
#include "editor/occurrences.h"

#include "editor/editing.h"
#include "editor/editorview.h"

#include <string.h>

#define OCCURRENCES_MS 100     /* after the selection settles */
#define MAX_NAME 200           /* longer selections are not looked for */
#define MAX_OCCURRENCES 10000  /* highlighted at most */
#define TAG "tcode-occurrence" /* VS Code's editor.selectionHighlightBackground */

struct Occurrences {
  Tab *tab;
  GtkTextBuffer *buffer; /* outlives the page: the tab holds a reference */
  GtkTextTag *tag;
  GArray *lines; /* the highlighted occurrences' lines; empty when none */
  guint update_id;
};

static gboolean is_name_char(gunichar c)
{
  return g_unichar_isalnum(c) || c == '_';
}

static gboolean is_name_byte(char c)
{
  return g_ascii_isalnum(c) || c == '_' || (c & 0x80); /* a letter of another alphabet may go on */
}

static void clear_occurrences(Occurrences *o)
{
  GtkTextIter start, end;

  if (o->lines->len == 0)
    return;
  gtk_text_buffer_get_bounds(o->buffer, &start, &end);
  gtk_text_buffer_remove_tag(o->buffer, o->tag, &start, &end);
  g_array_set_size(o->lines, 0);
  editor_view_show_problems(o->tab); /* the strip */
}

/* The selected name, or NULL when the selection is not exactly one whole name. */
static char *selected_name(Occurrences *o)
{
  GtkTextIter start, end, word_start, word_end;

  if (!gtk_text_buffer_get_selection_bounds(o->buffer, &start, &end) ||
      gtk_text_iter_get_line(&start) != gtk_text_iter_get_line(&end) ||
      gtk_text_iter_get_offset(&end) - gtk_text_iter_get_offset(&start) > MAX_NAME ||
      !is_name_char(gtk_text_iter_get_char(&start)) || !editor_word_at(&start, &word_start, &word_end) ||
      !gtk_text_iter_equal(&start, &word_start) || !gtk_text_iter_equal(&end, &word_end))
    return NULL;
  return gtk_text_buffer_get_text(o->buffer, &start, &end, TRUE);
}

/* Highlights every whole-word occurrence of the selected name, when it has others. */
static gboolean find_occurrences(gpointer data)
{
  Occurrences *o = data;
  char *name = selected_name(o), *text;
  GtkTextIter start, end;
  GArray *found;
  const char *from, *at;
  size_t length;
  glong chars = 0;

  o->update_id = 0;
  clear_occurrences(o);
  if (!name)
    return G_SOURCE_REMOVE;
  gtk_text_buffer_get_bounds(o->buffer, &start, &end);
  text = gtk_text_buffer_get_text(o->buffer, &start, &end, TRUE);
  length = strlen(name);
  found = g_array_new(FALSE, FALSE, sizeof(glong)); /* character offsets */
  for (from = text; found->len < MAX_OCCURRENCES && (at = strstr(from, name)); from = at + length) {
    chars += g_utf8_strlen(from, at - from);
    if ((at == text || !is_name_byte(at[-1])) && !is_name_byte(at[length]))
      g_array_append_val(found, chars);
    chars += g_utf8_strlen(at, length);
  }
  if (found->len > 1) {
    glong name_chars = g_utf8_strlen(name, -1);
    for (guint i = 0; i < found->len; i++) {
      gtk_text_buffer_get_iter_at_offset(o->buffer, &start, g_array_index(found, glong, i));
      end = start;
      gtk_text_iter_forward_chars(&end, (int)name_chars);
      gtk_text_buffer_apply_tag(o->buffer, o->tag, &start, &end);
      int line = gtk_text_iter_get_line(&start);
      if (o->lines->len == 0 || g_array_index(o->lines, int, o->lines->len - 1) != line)
        g_array_append_val(o->lines, line);
    }
    editor_view_show_problems(o->tab);
  }
  g_array_free(found, TRUE);
  g_free(text);
  g_free(name);
  return G_SOURCE_REMOVE;
}

static void on_occurrence_mark_set(GtkTextBuffer *buffer, const GtkTextIter *iter G_GNUC_UNUSED, GtkTextMark *mark,
                                   Occurrences *o)
{
  if (mark != gtk_text_buffer_get_insert(buffer) && mark != gtk_text_buffer_get_selection_bound(buffer))
    return;
  if (o->update_id)
    g_source_remove(o->update_id);
  o->update_id = g_timeout_add(OCCURRENCES_MS, find_occurrences, o);
}

/* An edit: the highlights would no longer be the name's (VS Code clears them too). */
static void on_occurrence_edit(GtkTextBuffer *buffer G_GNUC_UNUSED, Occurrences *o)
{
  clear_occurrences(o);
}

void occurrences_attach(Tab *tab)
{
  Occurrences *o = g_new0(Occurrences, 1);
  GdkRGBA background = { 0.68f, 0.84f, 1.0f, 0.15f }; /* #add6ff26 */

  o->tab = tab;
  o->buffer = GTK_TEXT_BUFFER(tab->buffer);
  o->lines = g_array_new(FALSE, FALSE, sizeof(int));
  o->tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(o->buffer), TAG);
  if (!o->tag)
    o->tag = gtk_text_buffer_create_tag(o->buffer, TAG, "background-rgba", &background, NULL);
  g_signal_connect(o->buffer, "mark-set", G_CALLBACK(on_occurrence_mark_set), o);
  g_signal_connect(o->buffer, "changed", G_CALLBACK(on_occurrence_edit), o);
  tab->view->occurrences = o;
}

void occurrences_lines(Tab *tab, GArray *lines)
{
  Occurrences *o = tab->view ? tab->view->occurrences : NULL;

  if (o)
    g_array_append_vals(lines, o->lines->data, o->lines->len);
}

void occurrences_free(Occurrences *o)
{
  if (!o)
    return;
  if (o->update_id)
    g_source_remove(o->update_id);
  g_signal_handlers_disconnect_by_data(o->buffer, o);
  g_array_free(o->lines, TRUE);
  g_free(o);
}
