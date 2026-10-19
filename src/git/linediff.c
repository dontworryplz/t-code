/*
 * Line differences (see linediff.h). The lines both versions start and end with are left out
 * first; what remains (an edit is small) is lined up by its longest common subsequence. Past
 * MAX_CELLS of that table its lines just count as modified: a gutter needs no perfect diff of a
 * file that was rewritten.
 */
#include "git/linediff.h"

#include <string.h>

#define MAX_CELLS (2000 * 2000)

/* A file's lines (no line breaks), sharing its text. */
typedef struct {
  char *text;
  GPtrArray *lines;
} Lines;

static void split(Lines *l, const char *text)
{
  l->text = g_strdup(text ? text : "");
  l->lines = g_ptr_array_new();
  for (char *p = l->text;;) {
    char *nl = strchr(p, '\n');
    g_ptr_array_add(l->lines, p);
    if (!nl)
      break;
    *nl = '\0';
    p = nl + 1;
  }
  /* A last line break ends the last line: no empty line after it. */
  if (l->lines->len > 1 && !*(char *)g_ptr_array_index(l->lines, l->lines->len - 1))
    g_ptr_array_set_size(l->lines, l->lines->len - 1);
}

static const char *line(Lines *l, guint i)
{
  return g_ptr_array_index(l->lines, i);
}

/* Marks a stretch: deleted old lines and added new ones (new from start, count of them). Where
 * both, the first new ones are modified ones. */
static void mark_hunk(GArray *marks, guint start, guint added, guint deleted)
{
  for (guint i = 0; i < added; i++)
    g_array_index(marks, guint8, start + i) = i < deleted ? LINE_MODIFIED : LINE_ADDED;
  if (added == 0 && deleted > 0 && marks->len > 0)
    g_array_index(marks, guint8, MIN(start, marks->len - 1)) = LINE_DELETED_ABOVE;
}

GArray *line_diff(const char *old_text, const char *new_text)
{
  Lines old, new;
  GArray *marks;
  guint head = 0, tail = 0, n, m;

  split(&old, old_text);
  split(&new, new_text);
  marks = g_array_new(FALSE, TRUE, sizeof(guint8));
  g_array_set_size(marks, new.lines->len);
  while (head < old.lines->len && head < new.lines->len && strcmp(line(&old, head), line(&new, head)) == 0)
    head++;
  while (tail < old.lines->len - head && tail < new.lines->len - head &&
         strcmp(line(&old, old.lines->len - 1 - tail), line(&new, new.lines->len - 1 - tail)) == 0)
    tail++;
  n = old.lines->len - head - tail;
  m = new.lines->len - head - tail;

  if (n == 0 || m == 0 || (gsize)n * m > MAX_CELLS) {
    mark_hunk(marks, head, m, n);
  } else {
    /* lcs[i][j]: the common lines of old[i..] and new[j..] (of the middle part). */
    guint16 *lcs = g_new0(guint16, (gsize)(n + 1) * (m + 1));
    guint i = 0, j = 0, start = 0, added = 0, deleted = 0;
#define LCS(a, b) lcs[(gsize)(a) * (m + 1) + (b)]
    for (int a = (int)n - 1; a >= 0; a--)
      for (int b = (int)m - 1; b >= 0; b--)
        LCS(a, b) = strcmp(line(&old, head + a), line(&new, head + b)) == 0 ? LCS(a + 1, b + 1) + 1
                                                                            : MAX(LCS(a + 1, b), LCS(a, b + 1));
    /* Walk it: a common line ends a hunk of deletions and additions. */
    while (i < n || j < m) {
      if (i < n && j < m && strcmp(line(&old, head + i), line(&new, head + j)) == 0) {
        mark_hunk(marks, head + start, added, deleted);
        i++, j++;
        start = j, added = deleted = 0;
      } else if (j < m && (i == n || LCS(i, j + 1) >= LCS(i + 1, j))) {
        j++, added++;
      } else {
        i++, deleted++;
      }
    }
    mark_hunk(marks, head + start, added, deleted);
#undef LCS
    g_free(lcs);
  }
  g_ptr_array_free(old.lines, TRUE);
  g_ptr_array_free(new.lines, TRUE);
  g_free(old.text);
  g_free(new.text);
  return marks;
}
