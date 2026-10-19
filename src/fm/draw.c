/* Drawing the tree, the bottom line (prompt or message) and the output buffer. */
#include "fm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

/* Output buffer ------------------------------------------------------------------------------ */

void buf_add(Buf *b, const char *s, size_t n)
{
  if (b->len + n > b->cap) {
    b->cap = (b->len + n) * 2 + 256;
    b->data = realloc(b->data, b->cap);
    if (!b->data)
      abort();
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
}

void buf_puts(Buf *b, const char *s)
{
  buf_add(b, s, strlen(s));
}

void buf_printf(Buf *b, const char *fmt, ...)
{
  char tmp[256];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  buf_add(b, tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static int next_char(const char *s, size_t *bytes)
{
  wchar_t wc;
  mbstate_t st = { 0 };
  size_t n = mbrtowc(&wc, s, 4, &st);

  if (n == 0 || n == (size_t)-1 || n == (size_t)-2) {
    *bytes = 1;
    return 1;
  }
  *bytes = n;
  int w = wcwidth(wc);
  return w >= 0 ? w : 1;
}

/* Appends at most `cols` columns of UTF-8 text s; returns the columns used. */
int buf_cols(Buf *b, const char *s, int cols)
{
  int used = 0;

  for (const char *p = s; *p;) {
    size_t n;
    int w = next_char(p, &n);
    if (used + w > cols)
      break;
    buf_add(b, p, n);
    used += w;
    p += n;
  }
  return used;
}

/* Writes the buffer to the terminal and empties it. */
void buf_write(Buf *b)
{
  if (write(STDOUT_FILENO, b->data, b->len) < 0) {
    /* Nothing useful to do: the terminal is gone. */
  }
  free(b->data);
  *b = (Buf){ 0 };
}

int utf8_cols(const char *s)
{
  int cols = 0;

  for (const char *p = s; *p;) {
    size_t n;
    cols += next_char(p, &n);
    p += n;
  }
  return cols;
}

/* The bottom line: a note until the next key. */
static void set_message(bool error, const char *fmt, va_list ap)
{
  vsnprintf(fm.message, sizeof fm.message, fmt, ap);
  fm.message_is_error = error;
}

void message(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  set_message(false, fmt, ap);
  va_end(ap);
}

void error_message(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  set_message(true, fmt, ap);
  va_end(ap);
}

/* Layout ------------------------------------------------------------------------------------- */

void update_size(void)
{
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    fm.width = ws.ws_col;
    fm.height = ws.ws_row;
  } else {
    fm.width = 30;
    fm.height = 24;
  }
}

int body_height(bool bottom_line)
{
  int h = fm.height - 1 - (bottom_line ? 1 : 0);
  return h > 0 ? h : 1;
}

/* File type icons --------------------------------------------------------------------------- */

/* Nerd Font glyphs; Cascadia Code has none, fontconfig falls back to an installed Nerd Font. */
typedef struct {
  const char *glyph;
  const char *color;
} Icon;

#define ICON_COLOR(r, g, b) "\x1b[38;2;" #r ";" #g ";" #b "m"

static const Icon icon_folder = { "", ICON_COLOR(220, 182, 122) };
static const Icon icon_folder_open = { "", ICON_COLOR(220, 182, 122) };
static const Icon icon_file = { "", ICON_COLOR(150, 150, 150) };

static const Icon icon_c = { "", ICON_COLOR(89, 158, 255) };
static const Icon icon_cpp = { "", ICON_COLOR(243, 75, 125) };
static const Icon icon_h = { "", ICON_COLOR(160, 116, 196) };
static const Icon icon_make = { "", ICON_COLOR(109, 128, 134) };
static const Icon icon_config = { "", ICON_COLOR(109, 128, 134) };
static const Icon icon_markdown = { "", ICON_COLOR(66, 165, 245) };
static const Icon icon_json = { "", ICON_COLOR(203, 203, 65) };
static const Icon icon_shell = { "", ICON_COLOR(137, 224, 81) };
static const Icon icon_python = { "", ICON_COLOR(255, 188, 3) };
static const Icon icon_lua = { "", ICON_COLOR(81, 160, 207) };
static const Icon icon_js = { "", ICON_COLOR(203, 203, 65) };
static const Icon icon_ts = { "", ICON_COLOR(81, 154, 186) };
static const Icon icon_html = { "", ICON_COLOR(228, 77, 38) };
static const Icon icon_css = { "", ICON_COLOR(66, 165, 245) };
static const Icon icon_rust = { "", ICON_COLOR(222, 165, 132) };
static const Icon icon_go = { "", ICON_COLOR(81, 154, 186) };
static const Icon icon_java = { "", ICON_COLOR(204, 62, 68) };
static const Icon icon_image = { "", ICON_COLOR(160, 116, 196) };
static const Icon icon_archive = { "", ICON_COLOR(236, 165, 23) };
static const Icon icon_pdf = { "", ICON_COLOR(230, 70, 60) };
static const Icon icon_text = { "", ICON_COLOR(137, 224, 81) };
static const Icon icon_binary = { "", ICON_COLOR(150, 150, 150) };
static const Icon icon_git = { "", ICON_COLOR(241, 76, 40) };
static const Icon icon_license = { "", ICON_COLOR(203, 203, 65) };
static const Icon icon_lock = { "", ICON_COLOR(187, 187, 187) };

static const struct {
  const char *name;
  const Icon *icon;
} icon_names[] = {
  /* clang-format off */
  { "Makefile", &icon_make },        { "makefile", &icon_make },
  { "GNUmakefile", &icon_make },     { "CMakeLists.txt", &icon_make },
  { ".gitignore", &icon_git },       { ".gitattributes", &icon_git },
  { ".gitmodules", &icon_git },      { ".clang-format", &icon_config },
  { ".clang-tidy", &icon_config },   { ".clangd", &icon_config },
  { ".editorconfig", &icon_config }, { "LICENSE", &icon_license },
  { "LICENSE.md", &icon_license },   { "LICENSE.txt", &icon_license },
  { "COPYING", &icon_license },
  /* clang-format on */
};

static const struct {
  const char *ext;
  const Icon *icon;
} icon_exts[] = {
  /* clang-format off */
  { "c", &icon_c },         { "cpp", &icon_cpp },       { "cc", &icon_cpp },
  { "cxx", &icon_cpp },     { "c++", &icon_cpp },       { "h", &icon_h },
  { "hpp", &icon_h },       { "hh", &icon_h },          { "hxx", &icon_h },
  { "inl", &icon_h },       { "mk", &icon_make },       { "cmake", &icon_make },
  { "md", &icon_markdown }, { "markdown", &icon_markdown },
  { "json", &icon_json },   { "yaml", &icon_config },   { "yml", &icon_config },
  { "toml", &icon_config }, { "ini", &icon_config },    { "conf", &icon_config },
  { "cfg", &icon_config },  { "xml", &icon_config },    { "desktop", &icon_config },
  { "sh", &icon_shell },    { "bash", &icon_shell },    { "zsh", &icon_shell },
  { "py", &icon_python },   { "lua", &icon_lua },       { "js", &icon_js },
  { "mjs", &icon_js },      { "ts", &icon_ts },         { "html", &icon_html },
  { "htm", &icon_html },    { "css", &icon_css },       { "rs", &icon_rust },
  { "go", &icon_go },       { "java", &icon_java },     { "png", &icon_image },
  { "jpg", &icon_image },   { "jpeg", &icon_image },    { "gif", &icon_image },
  { "svg", &icon_image },   { "ico", &icon_image },     { "webp", &icon_image },
  { "bmp", &icon_image },   { "zip", &icon_archive },   { "tar", &icon_archive },
  { "gz", &icon_archive },  { "xz", &icon_archive },    { "bz2", &icon_archive },
  { "zst", &icon_archive }, { "7z", &icon_archive },    { "pdf", &icon_pdf },
  { "txt", &icon_text },    { "log", &icon_text },      { "o", &icon_binary },
  { "a", &icon_binary },    { "so", &icon_binary },     { "bin", &icon_binary },
  { "lock", &icon_lock },
  /* clang-format on */
};

static const Icon *icon_of(const Row *row)
{
  if (row->dir)
    return is_expanded(row->path) >= 0 ? &icon_folder_open : &icon_folder;
  for (int i = 0; i < N_ELEMENTS(icon_names); i++)
    if (strcmp(row->name, icon_names[i].name) == 0)
      return icon_names[i].icon;
  const char *ext = strrchr(row->name, '.');
  if (ext && ext != row->name)
    for (int i = 0; i < N_ELEMENTS(icon_exts); i++)
      if (strcasecmp(ext + 1, icon_exts[i].ext) == 0)
        return icon_exts[i].icon;
  return &icon_file;
}

static void draw_row(Buf *b, const Row *row, int index)
{
  int cols = fm.width;
  bool selected = index == fm.sel && !fm.dragging;
  bool drop = fm.dragging && index == fm.drop_row;
  bool cut = fm.clip_cut && strcmp(fm.clip, row->path) == 0;

  if (selected)
    buf_puts(b, fm.focused ? BG_SELECTED : BG_INACTIVE);
  else if (drop)
    buf_puts(b, BG_DROP);
  for (int i = 0; i < row->depth && cols >= 2; i++, cols -= 2)
    buf_puts(b, "  ");
  if (cols >= 2) {
    if (row->dir) {
      buf_puts(b, COLOR_CHEVRON);
      buf_puts(b, is_expanded(row->path) >= 0 ? "▾ " : "▸ ");
    } else {
      buf_puts(b, "  ");
    }
    cols -= 2;
  }
  if (cols >= 2) {
    const Icon *icon = icon_of(row);
    buf_puts(b, cut ? COLOR_DIM : icon->color);
    buf_puts(b, icon->glyph);
    buf_add(b, " ", 1);
    cols -= 2;
  }
  const char *badge = row->dir ? NULL : git_badge(row->git);
  int name_cols = badge && cols > 2 ? cols - 2 : cols;
  buf_puts(b, cut ? COLOR_DIM : git_color(row->git));
  cols -= buf_cols(b, row->name, name_cols);
  if (badge && cols >= 2) {
    while (cols > 2) {
      buf_add(b, " ", 1);
      cols--;
    }
    buf_puts(b, badge);
    buf_add(b, " ", 1);
    cols -= 2;
  }
  if (selected || drop)
    while (cols-- > 0)
      buf_add(b, " ", 1);
  buf_puts(b, RESET "\x1b[K");
}

/* The tree, and the bottom line: an input field when prompt is given, otherwise fm.message, if any. */
void draw(const char *prompt, const char *input)
{
  bool bottom = prompt || fm.message[0];
  int body = body_height(bottom);
  Buf b = { 0 };

  if (fm.follow) {
    if (fm.sel < fm.top)
      fm.top = fm.sel;
    if (fm.sel >= fm.top + body)
      fm.top = fm.sel - body + 1;
  }
  if (fm.top > fm.nrows - body)
    fm.top = fm.nrows - body;
  if (fm.top < 0)
    fm.top = 0;

  buf_puts(&b, "\x1b[?25l\x1b[1;1H" RESET "\x1b[1m" COLOR_TEXT);
  if (fm.dragging && fm.drop_row < 0)
    buf_puts(&b, BG_DROP); /* dropping on the root */
  const char *name = strrchr(fm.root, '/');
  int used = buf_cols(&b, name && name[1] ? name + 1 : fm.root, fm.width);
  if (fm.dragging && fm.drop_row < 0)
    while (used++ < fm.width)
      buf_add(&b, " ", 1);
  buf_puts(&b, RESET "\x1b[K");

  for (int i = 0; i < body; i++) {
    int r = fm.top + i;
    buf_printf(&b, "\x1b[%d;1H", i + 2);
    if (r < fm.nrows)
      draw_row(&b, &fm.rows[r], r);
    else
      buf_puts(&b, RESET "\x1b[K");
  }

  if (bottom) {
    buf_printf(&b, "\x1b[%d;1H" RESET, fm.height);
    if (prompt) {
      int cols = fm.width - buf_cols(&b, prompt, fm.width);
      /* Show the end of the input when it does not fit. */
      const char *tail = input;
      int len = utf8_cols(input);
      while (len >= cols && *tail) {
        size_t n;
        len -= next_char(tail, &n); /* a wide character takes two columns */
        tail += n;
      }
      buf_cols(&b, tail, cols);
      buf_puts(&b, "\x1b[K\x1b[?25h");
    } else {
      buf_puts(&b, fm.message_is_error ? COLOR_ERROR : COLOR_TEXT);
      buf_cols(&b, fm.message, fm.width);
      buf_puts(&b, RESET "\x1b[K");
    }
  }
  buf_write(&b);
}
