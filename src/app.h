/* t-code: shared types and the functions the source files call on each other. */
#pragma once

#include <gtk/gtk.h>
#include <vte/vte.h>

typedef struct App App;
typedef struct Preview Preview;
typedef struct Picker Picker;

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
    GMenu *recent_menu;
    VteTerminal *fm;
    VteTerminal *shell;
    GPid fm_pid, shell_pid;
    Picker *picker;
    struct {
        GtkLabel *branch, *problems, *build, *position;
    } status;
    char *root;
    char *socket_path;
    char *fm_program;   /* t-code-fm */
    char *micro_config; /* micro -config-dir */
    char *temp_dir;     /* old file versions for comparisons */
    GSocketService *service;
    double font_scale;
    gboolean closing;
    gboolean plain;     /* syntax coloring off (F7) */
};

/* terminal.c */
VteTerminal *new_terminal(App *t);
void spawn(App *t, VteTerminal *term, char **argv, char **extra_env, GPid *pid);
void create_fixed_terminals(App *t);
void start_fixed_terminals(App *t);
void restart_fixed_terminals(App *t);
void run_in_shell(App *t, const char *command);

/* tabs.c */
int tab_count(App *t);           /* tabs in all groups */
Tab *tab_nth(App *t, int index); /* 0 .. tab_count() - 1, the first group first */
Tab *current_tab(App *t);        /* the current tab of the active group */
Tab *find_tab(App *t, TabKind kind, const char *path);
int tab_group(Tab *tab);
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path);
void move_tab(Tab *tab, int group);
void update_tab_label(Tab *tab);
void select_tab(Tab *tab);
void step_tab(App *t, int step);
void focus_current_tab(App *t);
void focus_group(App *t, int group);
void close_all_tabs(App *t);
void create_groups(App *t);
void update_editor_area(App *t);
Tab *open_editor(App *t, const char *path, int line, int column, gboolean readonly, char **extra_env);
void open_file(App *t, const char *path);
void open_file_at(App *t, const char *path, int line, int column);
void open_to_side(App *t, const char *path);
void feed_editor(Tab *tab, const char *keys);
void editor_command(Tab *tab, const char *command);
void set_split(App *t, gboolean split);
void toggle_syntax(App *t);
gboolean any_unsaved(App *t);

/* commands.c */
void add_shortcuts(App *t);
void command_palette(App *t);
typedef struct {
    const char *name;
    const char *keys; /* as shown; NULL when there is no shortcut */
} KeyHelp;
const KeyHelp *window_keys(int *n);

/* keybinds.c */
void show_keybinds(App *t);

/* picker.c: the popup list behind Ctrl+P, the command palette, search and file history.
 * query() is called with each new text and fills the list with picker_add(), now or later
 * (results for an older generation are dropped). */
typedef void (*PickerQuery)(App *t, const char *query, guint generation);
typedef void (*PickerChoose)(App *t, const char *payload);
void picker_init(App *t, GtkOverlay *overlay);
void picker_open(App *t, const char *placeholder, PickerQuery query, PickerChoose choose);
void picker_add(App *t, guint generation, const char *label, const char *detail, const char *payload);
void picker_message(App *t, guint generation, const char *text);
int fuzzy_score(const char *query, const char *text);

/* quickopen.c, search.c */
void quick_open_show(App *t);
void search_show(App *t);

/* status.c */
GtkWidget *status_bar(App *t);
void update_status(App *t);
void start_status_updates(App *t);

/* project.c */
typedef void (*ConfirmedFunc)(App *t, const char *arg);
void unless_unsaved(App *t, const char *question, const char *button, ConfirmedFunc then, const char *arg);
void update_title(App *t);
void add_project_actions(App *t);
void rebuild_recent_menu(App *t);
void choose_folder(App *t);
void open_project(App *t, const char *root);

/* socket.c */
void start_socket(App *t);
void stop_socket(App *t);

/* gitcompare.c */
void compare_with_head(App *t, const char *path);
void file_history(App *t, const char *path);
void stop_comparing(App *t);
void forget_comparison(Tab *old_version);
void remove_temp_dir(App *t);
const char *comparable_path(App *t);

/* run.c */
void build_project(App *t, gboolean run);
char *project_summary(App *t);

/* session.c */
void session_apply_window(App *t);
gboolean session_restore_tabs(App *t);
void session_save(App *t);
void session_add_recent(App *t);
char **session_recent(void);

/* preview.c */
gboolean is_markdown(const char *path);
void open_preview(App *t, const char *path);
void preview_set_zoom(Tab *tab, double scale);
void preview_free(Preview *preview);

/* t-code.c */
void set_font_scale(App *t, double scale);
void set_pane_visible(App *t, GtkWidget *pane, gboolean visible);
void toggle_pane(App *t, GtkWidget *pane);
