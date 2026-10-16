/* t-code: shared types and the functions the source files call on each other. */
#pragma once

#include <gtk/gtk.h>
#include <vte/vte.h>

typedef struct App App;
typedef struct Preview Preview;
typedef struct QuickOpen QuickOpen;

typedef enum {
    TAB_EDITOR,  /* a terminal running micro */
    TAB_PREVIEW, /* a rendered markdown file */
} TabKind;

typedef struct {
    App *app;
    TabKind kind;
    GtkWidget *page;   /* the notebook page: the terminal or the preview */
    VteTerminal *term; /* editors only */
    Preview *preview;  /* previews only */
    GtkLabel *label;
    char *path;
    gboolean modified;
    GPid pid;
} Tab;

/* The editor area holds two tab groups side by side; the second one only while split. */
#define GROUPS 2

struct App {
    GtkWindow *window;
    GtkPaned *hpaned, *vpaned, *split;
    GtkStack *editor_area;
    GtkNotebook *groups[GROUPS];
    int active;                 /* the group new tabs go to */
    GtkToggleButton *split_button;
    GtkLabel *build_label;      /* status bar: what Build / Run would build */
    GMenu *recent_menu;
    VteTerminal *fm;
    VteTerminal *shell;
    GPid fm_pid, shell_pid;
    QuickOpen *quick_open;
    char *root;
    char *socket_path;
    char *fm_program;   /* t-code-fm */
    char *micro_config; /* micro -config-dir */
    GSocketService *service;
    double font_scale;
    gboolean closing;
    gboolean plain;     /* syntax coloring off (F7) */
};

/* t-code.c */
int tab_count(App *t);           /* tabs in all groups */
Tab *tab_nth(App *t, int index); /* 0 .. tab_count() - 1, the first group first */
Tab *current_tab(App *t);        /* the current tab of the active group */
int tab_group(Tab *tab);
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path);
void update_tab_label(Tab *tab);
void select_tab(Tab *tab);
void focus_current_tab(App *t);
void open_file(App *t, const char *path);
void set_split(App *t, gboolean split);
void set_font_scale(App *t, double scale);
void set_pane_visible(App *t, GtkWidget *pane, gboolean visible);
void run_in_shell(App *t, const char *command);
void update_status(App *t);

/* run.c */
void build_project(App *t, gboolean run);
char *project_summary(App *t);

/* session.c */
void session_apply_window(App *t);
gboolean session_restore_tabs(App *t);
void session_save(App *t);
void session_add_recent(App *t);
char **session_recent(void);

/* quickopen.c */
void quick_open_init(App *t, GtkOverlay *overlay);
void quick_open_show(App *t);

/* preview.c */
gboolean is_markdown(const char *path);
void open_preview(App *t, const char *path);
void preview_set_zoom(Tab *tab, double scale);
void preview_free(Preview *preview);
