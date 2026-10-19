/*
 * An editor tab's page: [ GtkSourceView (scrolled) | GtkSourceMap minimap ], its look (the Dark
 * Modern style scheme, the editor font) and the settings that shape it (tab size, word wrap, ...).
 * Loading and saving the file are in editor.c, which keeps their state here too.
 */
#pragma once

#include "app.h"

typedef void (*EditorSavedFunc)(gpointer data, gboolean ok);

/* A save's caller, told when it is done. */
typedef struct {
  EditorSavedFunc func;
  gpointer data;
} SaveWaiter;

/* The state of one editor's page; tab->view. */
typedef struct FindBar FindBar;       /* editor/findbar.c */
typedef struct DiffGutter DiffGutter; /* editor/diffgutter.c */

struct EditorView {
  Tab *tab;
  GtkWidget *overlay;     /* the text, with the find bar over it */
  GtkWidget *map;         /* the minimap */
  GtkWidget *overview;    /* the problems strip: the whole file's errors and warnings */
  GPtrArray *diagnostics; /* lsp/diagnostics.c */
  FindBar *find;          /* made on the first Ctrl+F */
  DiffGutter *diff;
  struct SignatureHelp *signature; /* lsp/signature.c */
  struct Semantic *semantic;       /* lsp/semantic.c */
  char *closers;                   /* closing brackets and quotes typed for the user (editing.c), from the
                            * cursor to closers_end: typing one steps over it, Backspace takes the pair */
  GtkTextMark *closers_end;
  GtkSourceFile *file;
  GFileMonitor *monitor; /* the file changed on disk: reloaded when nothing is unsaved */
  GCancellable *cancel;  /* cancelled when the tab goes, for loads and saves in flight */
  gboolean readonly;
  gboolean loading;
  guint changes;               /* counts changes, to know whether a save wrote the latest text */
  char *etag;                  /* the file on disk as last loaded or saved: other changes reload it */
  gboolean saving, save_again; /* a save is in flight; another one is due after it */
  gboolean save_failed;        /* told the user once; quiet until a save works again */
  GArray *next_waiters;        /* SaveWaiter: for the save after the one in flight */
  guint autosave_id, reload_id, scroll_id;
  gboolean scroll_pending;    /* the cursor is scrolled to once the view is shown */
  gboolean lsp_pending;       /* loaded while hidden: the language server gets it once shown */
  int goto_line, goto_column; /* where the cursor goes once loaded (0: the start) */
  gboolean own_indent;        /* the file's own indentation (detect indentation) overrides the settings */
  gboolean own_tabs;
  int own_width;
};

/* An editor tab's page, empty until editor_view_init() fills it. */
GtkWidget *editor_page(void);

/* Fills tab->page with a new buffer and its view: tab->source, tab->buffer and tab->view (empty
 * until editor.c loads the file). */
void editor_view_init(Tab *tab, gboolean readonly);

/* Frees a tab's view: the file, its watch and timers. NULL is fine. */
void editor_view_free(EditorView *view);

/* Applies the settings (tab size, spaces, word wrap, minimap, code coloring) to an editor. */
void editor_view_apply_settings(Tab *tab);

/* The editor font (t->settings.font at t->font_scale) for all editors. */
void editor_view_set_font(App *t);

/* The problems strip draws the editor's diagnostics again. */
void editor_view_show_problems(Tab *tab);

/* Brings the cursor into view: when it is outside, the view scrolls to have it in the middle
 * (vertically; sideways only as far as needed), like VS Code's jumps. */
void editor_view_reveal_cursor(Tab *tab);

/* The language for path, from its name and text (a .h with C++ in it is C++). NULL: plain text. */
GtkSourceLanguage *editor_language(const char *path, const char *text);

/* The Dark Modern style scheme (styles/dark-modern.xml); the first call also sets up the CSS all
 * editors share. Markdown previews color their code blocks with it too. */
GtkSourceStyleScheme *editor_style_scheme(App *t);
