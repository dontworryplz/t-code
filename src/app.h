/*
 * t-code: the two types every part of the window shares.
 *   App  the whole window and its state (one per t-code process)
 *   Tab  one editor tab (micro in a terminal) or Markdown preview tab
 * Each module declares its functions in its own header next to its .c file; this one holds only
 * what they all need. The folders:
 *   main.c     start-up and shutdown
 *   window/    the window itself: layout, theme, terminals, status bar, shortcuts
 *   editor/    editor tabs: micro, the page around it (minimap, guides), Markdown previews
 *   search/    the Search panel
 *   git/       Source Control panel, git status, comparing with old versions
 *   popups/    the popup list (Ctrl+P, command palette)
 *   dialogs/   Settings, Keyboard Shortcuts, About
 *   project/   the project folder, the saved session, Build and Run
 *   ipc/       sockets: the file manager's commands, the shared language servers
 */
#pragma once

#include <gtk/gtk.h>
#include <vte/vte.h>

typedef struct App App;
typedef struct Preview Preview;       /* editor/preview.c */
typedef struct Picker Picker;         /* popups/picker.c */
typedef struct EditorView EditorView; /* editor/editorview.h */

typedef enum {
    TAB_EDITOR,  /* a terminal running micro */
    TAB_PREVIEW, /* a rendered markdown file */
} TabKind;

typedef struct {
    App *app;
    TabKind kind;
    GtkWidget *page;    /* the notebook page: the terminal or the preview */
    VteTerminal *term;  /* editors only */
    Preview *preview;   /* previews only */
    GtkLabel *label;
    char *path;
    char *title;        /* shown instead of the file name (an old version in a comparison) */
    char *compare_with; /* old versions only: the working file they are compared with */
    gboolean modified;
    int line, column;   /* cursor, errors and warnings: reported by micro (micro/plug/tcode) */
    int errors, warnings;
    int top, height, lines; /* first line shown (0-based), lines that fit, lines in the file */
    int tab_size, start_col; /* micro's tabsize for the file, first column shown */
    int visual_col;     /* the cursor's column from the line's start, tabs expanded (0-based) */
    EditorView *view;   /* editors only: minimap, scrollbar, control pipe */
    GPid pid;
    char **argv, **env; /* an editor whose micro starts when it is first shown, else NULL */
    guint start_id;     /* the start, due once the tab is still shown when the main loop is idle */
} Tab;

/* The editor area holds two tab groups side by side; the second one only while split. */
#define GROUPS 2

struct App {
    GtkWindow *window;
    GtkPaned *hpaned, *vpaned, *split;
    GtkWidget *sidebar;         /* left: tabs for the file manager, Source Control and Search */
    GtkStack *sidebar_stack;    /* pages "files", "scm" and "search" */
    GtkWidget *scm;
    GtkStack *editor_area;
    GtkNotebook *groups[GROUPS];
    int active;                 /* the group new tabs go to */
    GtkToggleButton *split_button;
    GMenu *recent_menu;
    VteTerminal *fm;
    VteTerminal *shell;
    GPid fm_pid, shell_pid;
    Picker *picker;
    struct {
        GtkLabel *branch, *problems, *build, *position;
    } status;
    char *root;
    char *git_top;      /* the work tree root of t->root's repository; NULL outside one */
    char *socket_path;
    char *fm_program;   /* t-code-fm */
    char *lsp_program;  /* t-code-lsp: connects an editor's lsp plugin to the shared servers */
    char *micro_config; /* micro -config-dir */
    char *icon_dir;     /* desktop/ in the build tree, holding the app icon */
    char *temp_dir;     /* old file versions for comparisons */
    GSocketService *service;
    double font_scale;
    gboolean closing;
    gboolean plain;     /* syntax coloring off (F7) */
    struct {
        gboolean minimap;
        gboolean reopen_last;    /* started from the app menu: open the last project */
        gboolean autosave;       /* changed editors save every second (autosave_tick) */
        gboolean tabs_to_spaces;
        int tab_size;
        gboolean detect_indent;  /* a file's own indentation overrides the two above */
        gboolean guides;         /* indent and bracket pair guides in editors */
        gboolean word_wrap;      /* long lines continue on the next row instead of running off */
        char *font;              /* editors and the shell, a Pango font description */
        char *fm_font;           /* the file manager */
    } settings;
};
