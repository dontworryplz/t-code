/*
 * t-code: the two types every part of the window shares.
 *   App  the whole window and its state (one per t-code process)
 *   Tab  one editor tab (GtkSourceView) or Markdown preview tab
 * Each module declares its functions in its own header next to its .c file; this one holds only
 * what they all need. The folders:
 *   main.c     start-up and shutdown
 *   window/    the window itself: layout, theme, terminals, status bar, shortcuts
 *   files/     the Files panel: the project's folder tree
 *   editor/    editor tabs: GtkSourceView, the page around it (minimap, guides), Markdown previews
 *   search/    the Search panel
 *   git/       Source Control panel, git status, comparing with old versions
 *   popups/    the popup list (Ctrl+P, command palette)
 *   dialogs/   Settings, Keyboard Shortcuts, About
 *   project/   the project folder, the saved session, Build and Run
 *   ipc/       the control socket ($TCODE_SOCKET): open files from scripts and tools
 *   lsp/       the language server: clangd's diagnostics, hover, completion, ...
 */
#pragma once

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <vte/vte.h>

typedef struct App App;
typedef struct Preview Preview;       /* editor/preview.c */
typedef struct Picker Picker;         /* popups/picker.c */
typedef struct EditorView EditorView; /* editor/editorview.h */
typedef struct Lsp Lsp;               /* lsp/client.c */

typedef enum {
  TAB_EDITOR,  /* a source file (GtkSourceView) */
  TAB_PREVIEW, /* a rendered markdown file */
} TabKind;

typedef struct Tab {
  App *app;
  TabKind kind;
  GtkWidget *page;       /* the editor's or the preview */
  GtkWidget *frame;      /* the notebook page when it is not page: a comparison's two sides */
  GtkSourceView *source; /* editors only */
  GtkSourceBuffer *buffer;
  Preview *preview; /* previews only */
  GtkLabel *label;
  char *path;
  char *title;          /* shown instead of the file name (an old version in a comparison) */
  char *compare_with;   /* old versions only: the working file they are compared with */
  struct Tab *old_side; /* a comparison's left side: the right one, in the same notebook page */
  gboolean temporary;   /* VS Code's preview editor: a click in Files opens it, the next one replaces it */
  gboolean modified;
  int line, column;     /* the cursor (1-based) */
  int errors, warnings; /* clangd's (lsp/diagnostics.c) */
  EditorView *view;     /* editors only: loading, saving, the minimap (editor/editorview.h) */
} Tab;

/* The editor area holds two tab groups side by side; the second one only while split. */
#define GROUPS 2
#define DOCKS 12 /* 0..3: left stacked, 4..7: bottom side-by-side, 8..11: right stacked */

struct App {
  GtkWindow *window;
  GtkPaned *hpaned, *rpaned, *vpaned, *split;
  GtkWidget *sidebar;           /* left vertical paned tree holding docks[0..3] */
  GtkWidget *bottom_bar;        /* bottom horizontal paned tree holding docks[4..7] */
  GtkWidget *right_sidebar;     /* right vertical paned tree holding docks[8..11] */
  GtkPaned *dock_paneds[3][3];  /* the 3 paneds per side (left, bottom, right) */
  GtkWidget *docks[DOCKS];      /* the 12 dock cards around the editor */
  GtkStack *dock_stacks[DOCKS]; /* the stack of panels in each dock */
  gboolean dock_collapsed[DOCKS];
  int dock_saved_pos[DOCKS];
  GtkStack *sidebar_stack; /* the dock stack currently holding t->scm */
  GtkWidget *scm;
  GtkStack *editor_area;
  GtkNotebook *groups[GROUPS];
  int active; /* the group new tabs go to */
  GtkToggleButton *split_button;
  GMenu *recent_menu;
  GtkWidget *files; /* the Files panel (files/files.c) */
  VteTerminal *shell;
  GPid shell_pid;
  Picker *picker;
  struct {
    GtkLabel *branch, *problems, *build, *position;
  } status;
  char *root;
  char *git_top; /* the work tree root of t->root's repository; NULL outside one */
  char *socket_path;
  char *styles_dir; /* the editor's style scheme (Dark Modern) */
  char *icon_dir;   /* desktop/ in the build tree, holding the app icon */
  char *temp_dir;   /* old file versions for comparisons */
  GSocketService *service;
  Lsp *lsp; /* the language server (clangd), while C or C++ files are open */
  double font_scale;
  gboolean closing;
  gboolean plain; /* syntax coloring off (F7) */
  struct {
    gboolean minimap;
    gboolean reopen_last; /* started from the app menu: open the last project */
    gboolean autosave;    /* a changed editor saves a second after its last change */
    gboolean tabs_to_spaces;
    int tab_size;
    gboolean detect_indent;  /* a file's own indentation overrides the two above */
    gboolean guides;         /* indent and bracket pair guides in editors */
    gboolean word_wrap;      /* long lines continue on the next row instead of running off */
    gboolean clang_tidy;     /* clangd also runs the project's .clang-tidy checks */
    gboolean format_on_save; /* Ctrl+S formats the file with clangd first */
    char *font;              /* editors and the shell, a Pango font description */
    char *files_font;        /* the Files panel */
  } settings;
};
