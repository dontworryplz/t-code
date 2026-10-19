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

static void lines_free(Lines *l)
{
  g_ptr_array_free(l->lines, TRUE);
  g_free(l->text);
}

static const char *line(Lines *l, guint i)
{
  return g_ptr_array_index(l->lines, i);
}

/* A stretch where the versions differ: deleted lines of old from old_start, added lines of new
 * from new_start. */
typedef void (*HunkFunc)(guint old_start, guint deleted, guint new_start, guint added, gpointer data);

/* Calls hunk for each stretch where old and new differ, in order. */
static void walk_hunks(Lines *old, Lines *new, HunkFunc hunk, gpointer data)
{
  guint head = 0, tail = 0, n, m;

  while (head < old->lines->len && head < new->lines->len && strcmp(line(old, head), line(new, head)) == 0)
    head++;
  while (tail < old->lines->len - head && tail < new->lines->len - head &&
         strcmp(line(old, old->lines->len - 1 - tail), line(new, new->lines->len - 1 - tail)) == 0)
    tail++;
  n = old->lines->len - head - tail;
  m = new->lines->len - head - tail;

  if (n == 0 || m == 0 || (gsize)n * m > MAX_CELLS) {
    if (n || m)
      hunk(head, n, head, m, data);
    return;
  }
  /* lcs[i][j]: the common lines of old[i..] and new[j..] (of the middle part). */
  guint16 *lcs = g_new0(guint16, (gsize)(n + 1) * (m + 1));
  guint i = 0, j = 0, start_i = 0, start_j = 0;
#define LCS(a, b) lcs[(gsize)(a) * (m + 1) + (b)]
  for (int a = (int)n - 1; a >= 0; a--)
    for (int b = (int)m - 1; b >= 0; b--)
      LCS(a, b) = strcmp(line(old, head + a), line(new, head + b)) == 0 ? LCS(a + 1, b + 1) + 1
                                                                        : MAX(LCS(a + 1, b), LCS(a, b + 1));
  /* Walk it: a common line ends a stretch of deletions and additions. */
  while (i < n || j < m) {
    if (i < n && j < m && strcmp(line(old, head + i), line(new, head + j)) == 0) {
      if (i > start_i || j > start_j)
        hunk(head + start_i, i - start_i, head + start_j, j - start_j, data);
      i++, j++;
      start_i = i, start_j = j;
    } else if (j < m && (i == n || LCS(i, j + 1) >= LCS(i + 1, j))) {
      j++;
    } else {
      i++;
    }
  }
  if (i > start_i || j > start_j)
    hunk(head + start_i, i - start_i, head + start_j, j - start_j, data);
#undef LCS
  g_free(lcs);
}

/* HunkFunc for line_diff(): where both, the first added lines are modified ones; deleted lines
 * alone mark the line after them. */
static void mark_hunk(guint old_start G_GNUC_UNUSED, guint deleted, guint new_start, guint added, gpointer data)
{
  GArray *marks = data;

  for (guint i = 0; i < added; i++)
    g_array_index(marks, guint8, new_start + i) = i < deleted ? LINE_MODIFIED : LINE_ADDED;
  if (added == 0 && deleted > 0 && marks->len > 0)
    g_array_index(marks, guint8, MIN(new_start, marks->len - 1)) = LINE_DELETED_ABOVE;
}

GArray *line_diff(const char *old_text, const char *new_text)
{
  Lines old, new;
  GArray *marks = g_array_new(FALSE, TRUE, sizeof(guint8));

  split(&old, old_text);
  split(&new, new_text);
  g_array_set_size(marks, new.lines->len);
  walk_hunks(&old, &new, mark_hunk, marks);
  lines_free(&old);
  lines_free(&new);
  return marks;
}

typedef struct {
  GArray *gaps;
  gboolean old_side;
  guint lines; /* in the side the gaps are for */
} Gaps;

/* HunkFunc for line_diff_gaps(): the side with fewer lines in a stretch gets a gap of the
 * difference, below the line before the stretch (above line 0 at the start). */
static void gap_hunk(guint old_start, guint deleted, guint new_start, guint added, gpointer data)
{
  Gaps *g = data;
  guint mine = g->old_side ? deleted : added, theirs = g->old_side ? added : deleted;
  guint at = (g->old_side ? old_start : new_start) + mine;
  DiffGap gap = { 0, (int)(theirs - mine), at == 0 };

  if (theirs <= mine || g->lines == 0)
    return;
  gap.line = at > 0 ? (int)MIN(at - 1, g->lines - 1) : 0;
  g_array_append_val(g->gaps, gap);
}

GArray *line_diff_gaps(const char *old_text, const char *new_text, gboolean for_old_side)
{
  Lines old, new;
  Gaps g = { g_array_new(FALSE, FALSE, sizeof(DiffGap)), for_old_side, 0 };

  split(&old, old_text);
  split(&new, new_text);
  g.lines = for_old_side ? old.lines->len : new.lines->len;
  walk_hunks(&old, &new, gap_hunk, &g);
  lines_free(&old);
  lines_free(&new);
  return g.gaps;
}
