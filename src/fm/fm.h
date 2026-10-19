/*
 * t-code-fm: the file manager pane of t-code, a tree view of the current directory that runs in
 * a terminal. It talks to t-code over the unix socket named by $TCODE_SOCKET, one line per
 * request (see src/ipc/socket.c): open, open-side, preview, project, compare, history, terminal, copy.
 *
 *   main.c   the main loop          tree.c   the folder tree        git.c    git state colors
 *   draw.c   drawing                input.c  keys and mouse         menu.c   right-click menu
 *   ops.c    file operations        watch.c  refreshing on changes (inotify)
 */
#pragma once

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

/* VS Code Dark Modern explorer colors. */
#define COLOR_TEXT "\x1b[38;2;204;204;204m"
#define COLOR_DIM "\x1b[38;2;128;128;128m"
#define COLOR_CHEVRON "\x1b[38;2;197;197;197m"
#define COLOR_ERROR "\x1b[38;2;241;76;76m"
#define BG_SELECTED "\x1b[48;2;4;57;94m"  /* focused */
#define BG_INACTIVE "\x1b[48;2;55;55;61m" /* not focused */
#define BG_DROP "\x1b[48;2;56;56;56m"     /* the folder a dragged file would go into */
#define BG_MENU "\x1b[48;2;31;31;31m"
#define COLOR_MENU_BORDER "\x1b[38;2;69;69;69m"
#define RESET "\x1b[0m"

#define N_ELEMENTS(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct {
  char *path;
  const char *name; /* points into path */
  int depth;
  bool dir;
  char git; /* 0, or a git state: M(odified) A(dded) U(ntracked) D(eleted) C(onflict) I(gnored) */
} Row;

typedef struct {
  char *path; /* absolute; ignored folders end without the slash */
  char state;
} GitEntry;

typedef struct {
  char root[PATH_MAX];
  Row *rows;
  int nrows, cap;
  char **expanded;
  int nexpanded;
  bool show_hidden;
  bool focused;
  bool follow; /* keep the selection on screen; off after scrolling with the wheel */
  int sel, top;
  int width, height;
  char message[512];
  bool message_is_error;
  GitEntry *git;
  int ngit;
  char git_dir[PATH_MAX]; /* the repository's .git folder, "" outside a repository */
  char clip[PATH_MAX];    /* cut or copied file, "" when none */
  bool clip_cut;
  int press_row; /* left button down on this row, -1 when not */
  int drop_row;  /* while dragging: the row under the pointer, -1 outside the rows */
  bool dragging;
  struct termios saved;
} Fm;

extern Fm fm;

/* draw.c */
typedef struct {
  char *data;
  size_t len, cap;
} Buf;
void buf_add(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
int buf_cols(Buf *b, const char *s, int cols);
void buf_write(Buf *b);
int utf8_cols(const char *s);
void message(const char *fmt, ...);
void error_message(const char *fmt, ...);
void update_size(void);
int body_height(bool bottom_line);
void draw(const char *prompt, const char *input);

/* tree.c */
char *join(const char *dir, const char *name);
void parent_dir(const char *path, char *out, size_t cap);
int is_expanded(const char *path);
void set_expanded(const char *path, bool on);
void rebuild_rows(const char *keep);
void rebuild(const char *keep);
void reveal(const char *path);
const char *selected_path(void);
const char *relative(const char *path);
void move(int delta);
int row_at(int y); /* the row on screen line y, or -1 */

/* git.c */
void git_refresh(void);
char git_state_of(const char *path, bool dir);
const char *git_color(char state);
const char *git_badge(char state);

/* input.c */
int read_input(unsigned char *buf, size_t cap);
int escape_len(const unsigned char *buf, int n);
bool prompt(const char *label, char *out, size_t cap, const char *initial);
bool confirm(const char *question);
void handle_input(const unsigned char *buf, int n);

/* menu.c */
void context_menu(int row, int x, int y);

/* ops.c */
void send_to_tcode(const char *command, const char *arg);
void activate(void);
void collapse(void);
void target_dir(char *out, size_t cap);
void create(void);
void create_in(const char *dir, bool folder);
void rename_selected(void);
void delete_selected(void);
void clip_selected(bool cut);
void paste_into(const char *dir);
void move_into(const char *src, const char *dir);
bool is_markdown(const Row *row);

/* watch.c */
int watch_fd(void);
void watch_sync(void);
bool watch_read(void);

/* main.c */
extern volatile int resized;
