/*
 * An editor tab's page: [ terminal (indent guides drawn over it) | minimap | scrollbar ].
 * micro draws on the alternate screen, so the terminal has no scrollback of its own: micro
 * reports which lines it shows through the terminal title (editor.c) and t-code scrolls it
 * through the control pipe (control.c), which drives the scrollbar and the minimap.
 * The parts, each in its own file, share the EditorView below:
 *   editorview.c  the page, the saved copy of the file, scrolling and the scrollbar
 *   control.c     the named pipe to micro's plugin
 *   minimap.c     the minimap (lexer.c colors it) and clangd's problem lines on it
 *   guides.c      the indent guides
 */
#pragma once

#include "app.h"

#define TERM_PADDING 1 /* the terminal's padding (editorview.c's CSS), around its cells */

/* The state of one editor's page; tab->view. */
struct EditorView {
    Tab *tab;
    GtkWidget *minimap;
    GtkAdjustment *adjustment; /* the scrollbar's */
    gboolean updating;  /* setting the adjustment from a report, not from the user */
    gint64 scrolled_at; /* when t-code last scrolled the editor */
    int control_fd;     /* the control pipe (control.c), -1 without one */
    char *control_path;
    GFileMonitor *monitor;
    guint reload_id;
    char *text;         /* the file, as last saved */
    gsize size;
    GArray *starts;     /* offset of each line in text */
    GArray *in_comment; /* gboolean: the line starts inside a block comment */
    gboolean c_like;    /* C/C++ source: colored; anything else is drawn plain */
    char *problems;     /* as micro reported them: "e12,w30", or "-" */
    GArray *problem_lines; /* Problem */
    double grab;        /* while dragging the viewport: where in it the pointer is */
    gboolean dragging;
    GtkWidget *guides;  /* drawn over the terminal */
    guint text_version; /* counts loads of text */
    struct {
        guint version;
        int first, last, line;
    } enclosing;        /* the last enclosing_line() (guides.c) */
};

/* A line with clangd errors or warnings, marked on the minimap. */
typedef struct {
    int line; /* 0-based */
    gboolean error;
} Problem;

/* Building the page -------------------------------------------------------------------------- */

/* Builds the editor's page around term: [ terminal | ], the rest is added by editor_view_init. */
GtkWidget *editor_page(VteTerminal *term);

/* Adds the indent guides, the minimap, the scrollbar and the control pipe to an editor tab (its
 * page from editor_page). Returns the pipe's path for TCODE_CONTROL, or NULL. */
const char *editor_view_init(Tab *tab, int number);

/* Frees a tab's view; closing the pipe ends the plugin's reader. NULL is fine. */
void editor_view_free(EditorView *view);

/* Following micro ---------------------------------------------------------------------------- */

/* Follows tab->path (the minimap's copy of the file and the watch on it): call again when it
 * changes. */
void editor_view_set_path(Tab *tab);

/* Takes what micro reported (top, and tab->height, tab->lines) into the scrollbar and the minimap. */
void editor_view_update(Tab *tab, int top);

/* Scrolls the editor so that line top (0-based) is at the top, from the minimap or the
 * scrollbar. */
void editor_view_scroll(EditorView *v, int top);

/* Shows or hides the minimap (the setting). */
void editor_view_show_minimap(Tab *tab, gboolean show);

/* Draws the minimap and the guides again (code coloring or the guides setting changed). */
void editor_view_redraw(Tab *tab);
