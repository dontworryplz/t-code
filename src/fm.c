/*
 * t-code-fm: the file manager pane of t-code.
 *
 * A tree view of the current directory that runs inside a terminal. It talks to t-code over the
 * unix socket named by $TCODE_SOCKET, one line per request:
 *   open <absolute path>       open the file in an editor tab
 *   open-side <absolute path>  the same, in the second (side by side) tab group
 *   preview <absolute path>    open the markdown preview of the file
 *   project <absolute dir>     switch t-code to that folder
 *   copy <text>                put text on the clipboard
 *   terminal <absolute dir>    cd the shell pane there
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* VS Code Dark Modern explorer colors. */
#define COLOR_TEXT "\x1b[38;2;204;204;204m"
#define COLOR_CHEVRON "\x1b[38;2;197;197;197m"
#define COLOR_ERROR "\x1b[38;2;241;76;76m"
#define BG_SELECTED "\x1b[48;2;4;57;94m"   /* focused */
#define BG_INACTIVE "\x1b[48;2;55;55;61m"  /* not focused */
#define BG_MENU "\x1b[48;2;31;31;31m"
#define COLOR_MENU_BORDER "\x1b[38;2;69;69;69m"

/* VS Code's git decoration colors. */
#define COLOR_GIT_MODIFIED "\x1b[38;2;226;192;141m"
#define COLOR_GIT_ADDED "\x1b[38;2;129;184;139m"
#define COLOR_GIT_UNTRACKED "\x1b[38;2;115;201;145m"
#define COLOR_GIT_DELETED "\x1b[38;2;199;78;57m"
#define COLOR_GIT_CONFLICT "\x1b[38;2;228;103;107m"
#define COLOR_GIT_IGNORED "\x1b[38;2;140;140;140m"
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

static struct {
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
    GitEntry *git;
    int ngit;
    struct termios saved;
} fm = { .focused = true, .follow = true };

static volatile sig_atomic_t resized;

/* Output buffer ------------------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const char *s, size_t n)
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

static void buf_puts(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_add(b, tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* Appends at most `cols` columns of UTF-8 text s; returns the columns used. */
static int buf_cols(Buf *b, const char *s, int cols)
{
    int used = 0;
    const char *p = s;

    while (*p && used < cols) {
        const char *start = p++;
        while ((*p & 0xC0) == 0x80)
            p++;
        buf_add(b, start, p - start);
        used++;
    }
    return used;
}

static void message(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(fm.message, sizeof fm.message, fmt, ap);
    va_end(ap);
}

/* Tree --------------------------------------------------------------------------------------- */

static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    char *path = malloc(n + strlen(name) + 2);

    if (!path)
        abort();
    sprintf(path, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", name);
    return path;
}

static int is_expanded(const char *path)
{
    for (int i = 0; i < fm.nexpanded; i++)
        if (strcmp(fm.expanded[i], path) == 0)
            return i;
    return -1;
}

static void set_expanded(const char *path, bool on)
{
    int i = is_expanded(path);

    if (on && i < 0) {
        fm.expanded = realloc(fm.expanded, (fm.nexpanded + 1) * sizeof *fm.expanded);
        fm.expanded[fm.nexpanded++] = strdup(path);
    } else if (!on && i >= 0) {
        free(fm.expanded[i]);
        fm.expanded[i] = fm.expanded[--fm.nexpanded];
    }
}

static void push_row(Row row)
{
    if (fm.nrows == fm.cap) {
        fm.cap = fm.cap ? fm.cap * 2 : 64;
        fm.rows = realloc(fm.rows, fm.cap * sizeof *fm.rows);
        if (!fm.rows)
            abort();
    }
    fm.rows[fm.nrows++] = row;
}

static int compare_rows(const void *a, const void *b)
{
    const Row *x = a, *y = b;

    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    Row *list = NULL;
    int n = 0, cap = 0;
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!fm.show_hidden && e->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            list = realloc(list, cap * sizeof *list);
            if (!list)
                abort();
        }
        struct stat st;
        char *path = join(dir, e->d_name);
        list[n++] = (Row){
            .path = path,
            .name = strrchr(path, '/') + 1,
            .depth = depth,
            .dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode),
        };
    }
    closedir(d);
    qsort(list, n, sizeof *list, compare_rows);
    for (int i = 0; i < n; i++) {
        push_row(list[i]);
        if (list[i].dir && is_expanded(list[i].path) >= 0)
            scan(list[i].path, depth + 1);
    }
    free(list);
}

/* Git ------------------------------------------------------------------------------------------ */

/* Runs argv and returns its output (NUL-terminated, length in *len), or NULL if it failed. */
static char *run_capture(char *const argv[], size_t *len)
{
    int fds[2];
    pid_t pid;
    char *out = NULL;
    size_t n = 0, cap = 0;
    int status;

    if (pipe(fds) < 0)
        return NULL;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(fds[1], STDOUT_FILENO);
        if (null >= 0)
            dup2(null, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            out = realloc(out, cap);
            if (!out)
                abort();
        }
        ssize_t got = read(fds[0], out + n, cap - n - 1);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        n += got;
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(out);
        return NULL;
    }
    out[n] = '\0';
    *len = n;
    return out;
}

static char git_state(char x, char y)
{
    if (x == '?')
        return 'U';
    if (x == '!')
        return 'I';
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
        return 'C';
    if (x == 'D' || y == 'D')
        return 'D';
    if (x == 'A')
        return 'A';
    return 'M';
}

static int git_rank(char state)
{
    switch (state) {
    case 'C': return 5;
    case 'M': case 'D': return 4;
    case 'A': case 'U': return 3;
    case 'I': return 1;
    default: return 0;
    }
}

/* Reloads `git status` for the repository the root is in (nothing when it is not in one). */
static void git_refresh(void)
{
    char *top_argv[] = { "git", "-C", fm.root, "rev-parse", "--show-toplevel", NULL };
    char *status_argv[] = { "git", "-C", fm.root, "status", "--porcelain=v1", "-z",
                            "--untracked-files=all", "--ignored=matching", NULL };
    size_t top_len, len;
    char *top, *out;

    for (int i = 0; i < fm.ngit; i++)
        free(fm.git[i].path);
    fm.ngit = 0;

    top = run_capture(top_argv, &top_len);
    if (!top)
        return;
    top[strcspn(top, "\n")] = '\0';
    out = run_capture(status_argv, &len);
    for (size_t i = 0; out && i + 3 < len;) {
        char *entry = out + i;
        size_t entry_len = strlen(entry);
        char x = entry[0], y = entry[1];
        i += entry_len + 1;
        if (x == 'R' || x == 'C') /* the original name of a rename or copy follows */
            i += strlen(out + i) + 1;

        char *path = join(top, entry + 3);
        size_t plen = strlen(path);
        if (plen > 1 && path[plen - 1] == '/')
            path[plen - 1] = '\0';
        fm.git = realloc(fm.git, (fm.ngit + 1) * sizeof *fm.git);
        fm.git[fm.ngit++] = (GitEntry){ path, git_state(x, y) };
    }
    free(out);
    free(top);
}

/* The state shown for a row: its own, or for a folder the strongest among what is inside;
 * anything inside an ignored folder is ignored too. */
static char git_state_of(const char *path, bool dir)
{
    size_t n = strlen(path);
    char best = 0;

    for (int i = 0; i < fm.ngit; i++) {
        const GitEntry *e = &fm.git[i];
        size_t elen = strlen(e->path);
        char state = 0;

        if (strcmp(e->path, path) == 0)
            state = e->state;
        else if (dir && e->state != 'I' && elen > n && strncmp(e->path, path, n) == 0 && e->path[n] == '/')
            state = e->state == 'D' ? 'M' : e->state;
        else if (e->state == 'I' && n > elen && strncmp(path, e->path, elen) == 0 && path[elen] == '/')
            state = 'I';
        if (git_rank(state) > git_rank(best))
            best = state;
    }
    return best;
}

static const char *git_color(char state)
{
    switch (state) {
    case 'M': return COLOR_GIT_MODIFIED;
    case 'A': return COLOR_GIT_ADDED;
    case 'U': return COLOR_GIT_UNTRACKED;
    case 'D': return COLOR_GIT_DELETED;
    case 'C': return COLOR_GIT_CONFLICT;
    case 'I': return COLOR_GIT_IGNORED;
    default: return COLOR_TEXT;
    }
}

/* The letter VS Code shows at the end of a file's row. */
static const char *git_badge(char state)
{
    switch (state) {
    case 'M': return "M";
    case 'A': return "A";
    case 'U': return "U";
    case 'D': return "D";
    case 'C': return "!";
    default: return NULL;
    }
}

/* Re-reads the tree and puts the selection on `keep` when it is still there. */
static void rebuild(const char *keep)
{
    char *wanted = keep ? strdup(keep) : NULL;

    for (int i = 0; i < fm.nrows; i++)
        free(fm.rows[i].path);
    fm.nrows = 0;
    scan(fm.root, 0);
    git_refresh();
    for (int i = 0; i < fm.nrows; i++)
        fm.rows[i].git = git_state_of(fm.rows[i].path, fm.rows[i].dir);

    if (wanted) {
        for (int i = 0; i < fm.nrows; i++) {
            if (strcmp(fm.rows[i].path, wanted) == 0) {
                fm.sel = i;
                break;
            }
        }
        free(wanted);
    }
    if (fm.sel >= fm.nrows)
        fm.sel = fm.nrows - 1;
    if (fm.sel < 0)
        fm.sel = 0;
}

static const char *selected_path(void)
{
    return fm.nrows ? fm.rows[fm.sel].path : NULL;
}

/* Marks every directory between the root and path as expanded. */
static void reveal(const char *path)
{
    char buf[PATH_MAX];
    size_t rootlen = strlen(fm.root);

    snprintf(buf, sizeof buf, "%s", path);
    for (char *slash = strrchr(buf, '/'); slash && (size_t)(slash - buf) > rootlen;
         slash = strrchr(buf, '/')) {
        *slash = '\0';
        set_expanded(buf, true);
    }
}

/* Drawing ------------------------------------------------------------------------------------ */

static void update_size(void)
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

static int body_height(bool bottom_line)
{
    int h = fm.height - 1 - (bottom_line ? 1 : 0);
    return h > 0 ? h : 1;
}

static void draw_row(Buf *b, const Row *row, bool selected)
{
    int cols = fm.width;

    if (selected)
        buf_puts(b, fm.focused ? BG_SELECTED : BG_INACTIVE);
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
    const char *badge = row->dir ? NULL : git_badge(row->git);
    int name_cols = badge && cols > 2 ? cols - 2 : cols;
    buf_puts(b, git_color(row->git));
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
    if (selected)
        while (cols-- > 0)
            buf_add(b, " ", 1);
    buf_puts(b, RESET "\x1b[K");
}

/* prompt/input: the bottom line is an input field. Otherwise it shows fm.message, if any. */
static void draw(const char *prompt, const char *input)
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
    const char *name = strrchr(fm.root, '/');
    buf_cols(&b, name && name[1] ? name + 1 : fm.root, fm.width);
    buf_puts(&b, RESET "\x1b[K");

    for (int i = 0; i < body; i++) {
        int r = fm.top + i;
        buf_printf(&b, "\x1b[%d;1H", i + 2);
        if (r < fm.nrows)
            draw_row(&b, &fm.rows[r], r == fm.sel);
        else
            buf_puts(&b, RESET "\x1b[K");
    }

    if (bottom) {
        buf_printf(&b, "\x1b[%d;1H" RESET, fm.height);
        if (prompt) {
            int cols = fm.width - buf_cols(&b, prompt, fm.width);
            /* Show the end of the input when it does not fit. */
            const char *tail = input;
            int len = 0;
            for (const char *p = input; *p; p++)
                len += (*p & 0xC0) != 0x80;
            while (len >= cols && *tail) {
                tail++;
                while ((*tail & 0xC0) == 0x80)
                    tail++;
                len--;
            }
            buf_cols(&b, tail, cols);
            buf_puts(&b, "\x1b[K\x1b[?25h");
        } else {
            buf_puts(&b, COLOR_ERROR);
            buf_cols(&b, fm.message, fm.width);
            buf_puts(&b, RESET "\x1b[K");
        }
    }
    if (write(STDOUT_FILENO, b.data, b.len) < 0) {
        /* Nothing useful to do: the terminal is gone. */
    }
    free(b.data);
}

/* Terminal ----------------------------------------------------------------------------------- */

static void restore_terminal(void)
{
    static const char leave[] = "\x1b[?1004l\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l";

    if (write(STDOUT_FILENO, leave, sizeof leave - 1) < 0) {
        /* Ignore: the terminal is gone. */
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &fm.saved);
}

static void setup_terminal(void)
{
    static const char enter[] = "\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[?1004h\x1b[?25l";
    struct termios raw;

    tcgetattr(STDIN_FILENO, &fm.saved);
    atexit(restore_terminal);
    raw = fm.saved;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    if (write(STDOUT_FILENO, enter, sizeof enter - 1) < 0)
        exit(1);
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

/* Reads input; returns the byte count, 0 when interrupted by a resize, -1 at end of input. */
static int read_input(unsigned char *buf, size_t cap)
{
    ssize_t n = read(STDIN_FILENO, buf, cap);

    if (n < 0 && errno == EINTR) {
        if (resized) {
            resized = 0;
            update_size();
        }
        return 0;
    }
    return n > 0 ? (int)n : -1;
}

/* Length of the escape sequence at buf[0] (ESC), or 1 for a lone ESC. */
static int escape_len(const unsigned char *buf, int n)
{
    if (n < 2 || (buf[1] != '[' && buf[1] != 'O'))
        return 1;
    for (int i = 2; i < n; i++)
        if (buf[i] >= 0x40 && buf[i] <= 0x7E)
            return i + 1;
    return n;
}

/* Bottom-line input. Returns false when cancelled with Escape. */
static bool prompt(const char *label, char *out, size_t cap, const char *initial)
{
    unsigned char buf[256];

    snprintf(out, cap, "%s", initial ? initial : "");
    for (;;) {
        draw(label, out);
        int n = read_input(buf, sizeof buf);
        if (n < 0)
            exit(0);
        for (int i = 0; i < n; i++) {
            unsigned char c = buf[i];
            size_t len = strlen(out);
            if (c == '\r' || c == '\n') {
                return true;
            } else if (c == 0x1B) {
                int skip = escape_len(buf + i, n - i);
                if (skip == 1)
                    return false;
                i += skip - 1;
            } else if (c == 0x7F || c == 0x08) {
                while (len > 0 && (out[len - 1] & 0xC0) == 0x80)
                    out[--len] = '\0';
                if (len > 0)
                    out[--len] = '\0';
            } else if (c == 0x03 || c == 0x07) { /* Ctrl+C, Ctrl+G */
                return false;
            } else if (c >= 0x20 && len + 1 < cap) {
                out[len] = c;
                out[len + 1] = '\0';
            }
        }
    }
}

static bool confirm(const char *question)
{
    char answer[8];

    return prompt(question, answer, sizeof answer, NULL) && (answer[0] == 'y' || answer[0] == 'Y');
}

/* Actions ------------------------------------------------------------------------------------ */

static void send_to_tcode(const char *command, const char *arg)
{
    const char *socket_path = getenv("TCODE_SOCKET");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    if (!socket_path || strlen(socket_path) >= sizeof addr.sun_path) {
        message("TCODE_SOCKET is not set");
        return;
    }
    strcpy(addr.sun_path, socket_path);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        message("cannot reach t-code: %s", strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    if (dprintf(fd, "%s %s\n", command, arg) < 0)
        message("cannot reach t-code: %s", strerror(errno));
    close(fd);
}

static void activate(void)
{
    if (!fm.nrows)
        return;
    Row *row = &fm.rows[fm.sel];
    if (row->dir) {
        set_expanded(row->path, is_expanded(row->path) < 0);
        rebuild(row->path);
    } else {
        send_to_tcode("open", row->path);
    }
}

/* Collapses the selected folder, or else its parent (and moves there). */
static void collapse(void)
{
    if (!fm.nrows)
        return;
    Row *row = &fm.rows[fm.sel];
    if (row->dir && is_expanded(row->path) >= 0) {
        set_expanded(row->path, false);
        rebuild(row->path);
        return;
    }
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", row->path);
    *strrchr(parent, '/') = '\0';
    if (strcmp(parent, fm.root) != 0) {
        set_expanded(parent, false);
        rebuild(parent);
    }
}

/* The folder new files go into: the selected folder, or the selected file's folder. */
static void target_dir(char *out, size_t cap)
{
    if (!fm.nrows) {
        snprintf(out, cap, "%s", fm.root);
        return;
    }
    Row *row = &fm.rows[fm.sel];
    snprintf(out, cap, "%s", row->path);
    if (row->dir)
        set_expanded(row->path, true);
    else
        *strrchr(out, '/') = '\0';
}

static const char *relative(const char *path)
{
    size_t n = strlen(fm.root);

    if (strncmp(path, fm.root, n) == 0 && path[n] == '/')
        return path + n + 1;
    return strcmp(path, fm.root) == 0 ? "" : path;
}

static int make_dirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            int rc = mkdir(path, 0755);
            *p = '/';
            if (rc < 0 && errno != EEXIST)
                return -1;
        }
    }
    return mkdir(path, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* Asks for a name and creates it in dir. A name ending in "/" (or folder) makes a folder. */
static void create_in(const char *dir, bool folder)
{
    char label[PATH_MAX + 64], name[NAME_MAX * 4], path[PATH_MAX * 2];

    const char *rel = relative(dir);
    snprintf(label, sizeof label, "%s: %s%s", folder ? "New folder" : "New", rel, *rel ? "/" : "");
    if (!prompt(label, name, sizeof name, NULL) || !name[0])
        return;
    if (folder && name[strlen(name) - 1] != '/' && strlen(name) + 1 < sizeof name)
        strcat(name, "/");

    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len = strlen(path);
    if (path[len - 1] == '/') {
        while (len > 1 && path[len - 1] == '/')
            path[--len] = '\0';
        if (make_dirs(path) < 0)
            message("cannot create %s: %s", name, strerror(errno));
    } else {
        char *slash = strrchr(path, '/');
        *slash = '\0';
        int rc = make_dirs(path);
        *slash = '/';
        int fd = rc < 0 ? -1 : open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0)
            message("cannot create %s: %s", name, strerror(errno));
        else
            close(fd);
    }
    reveal(path);
    rebuild(path);
}

static void create(void)
{
    char dir[PATH_MAX];

    target_dir(dir, sizeof dir);
    create_in(dir, false);
}

static void rename_selected(void)
{
    char name[NAME_MAX * 4], dest[PATH_MAX * 2];

    if (!fm.nrows)
        return;
    Row *row = &fm.rows[fm.sel];
    if (!prompt("Rename to: ", name, sizeof name, row->name) || !name[0] || strchr(name, '/'))
        return;

    snprintf(dest, sizeof dest, "%.*s/%s", (int)(row->name - 1 - row->path), row->path, name);
    if (access(dest, F_OK) == 0) {
        message("%s already exists", name);
        return;
    }
    if (rename(row->path, dest) < 0) {
        message("cannot rename: %s", strerror(errno));
        return;
    }
    rebuild(dest);
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st, (void)type, (void)ftw;
    return remove(path);
}

static void delete_selected(void)
{
    char question[NAME_MAX * 4 + 64];

    if (!fm.nrows)
        return;
    Row *row = &fm.rows[fm.sel];
    snprintf(question, sizeof question, "Delete %s%s? (y/n) ", row->name,
             row->dir ? "/ and everything in it" : "");
    if (!confirm(question))
        return;
    int rc = row->dir ? nftw(row->path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) : unlink(row->path);
    if (rc < 0)
        message("cannot delete %s: %s", row->name, strerror(errno));
    rebuild(NULL);
}

static void move(int delta)
{
    fm.sel += delta;
    if (fm.sel >= fm.nrows)
        fm.sel = fm.nrows - 1;
    if (fm.sel < 0)
        fm.sel = 0;
    fm.follow = true;
}

/* Context menu ------------------------------------------------------------------------------- */

enum { ACT_NONE = -1, ACT_SEPARATOR, ACT_OPEN, ACT_OPEN_SIDE, ACT_PREVIEW, ACT_PROJECT, ACT_NEW_FILE, ACT_NEW_FOLDER, ACT_TERMINAL,
       ACT_COPY_PATH, ACT_COPY_RELATIVE, ACT_RENAME, ACT_DELETE };

typedef struct {
    const char *label;
    int action;
} MenuItem;

static const MenuItem row_menu[] = {
    { "Open", ACT_OPEN },
    { "Open to the Side", ACT_OPEN_SIDE },
    { "New File…", ACT_NEW_FILE },
    { "New Folder…", ACT_NEW_FOLDER },
    { "Open in Terminal", ACT_TERMINAL },
    { NULL, ACT_SEPARATOR },
    { "Copy Path", ACT_COPY_PATH },
    { "Copy Relative Path", ACT_COPY_RELATIVE },
    { NULL, ACT_SEPARATOR },
    { "Rename…", ACT_RENAME },
    { "Delete", ACT_DELETE },
};

static const MenuItem folder_menu[] = {
    { "Open", ACT_OPEN },
    { "Open Folder as Project", ACT_PROJECT },
    { "New File…", ACT_NEW_FILE },
    { "New Folder…", ACT_NEW_FOLDER },
    { "Open in Terminal", ACT_TERMINAL },
    { NULL, ACT_SEPARATOR },
    { "Copy Path", ACT_COPY_PATH },
    { "Copy Relative Path", ACT_COPY_RELATIVE },
    { NULL, ACT_SEPARATOR },
    { "Rename…", ACT_RENAME },
    { "Delete", ACT_DELETE },
};

/* The same, for markdown files. */
static const MenuItem markdown_menu[] = {
    { "Open", ACT_OPEN },
    { "Open to the Side", ACT_OPEN_SIDE },
    { "Open Preview", ACT_PREVIEW },
    { "New File…", ACT_NEW_FILE },
    { "New Folder…", ACT_NEW_FOLDER },
    { "Open in Terminal", ACT_TERMINAL },
    { NULL, ACT_SEPARATOR },
    { "Copy Path", ACT_COPY_PATH },
    { "Copy Relative Path", ACT_COPY_RELATIVE },
    { NULL, ACT_SEPARATOR },
    { "Rename…", ACT_RENAME },
    { "Delete", ACT_DELETE },
};

static const MenuItem empty_menu[] = {
    { "New File…", ACT_NEW_FILE },
    { "New Folder…", ACT_NEW_FOLDER },
    { "Open in Terminal", ACT_TERMINAL },
};

typedef struct {
    const MenuItem *items;
    int n, sel;
    int left, top, inner; /* 1-based screen position of the box; inner = width inside the border */
} Menu;

static int utf8_cols(const char *s)
{
    int n = 0;

    for (; *s; s++)
        n += (*s & 0xC0) != 0x80;
    return n;
}

/* Puts the box at the click, moved left or up when it would not fit. */
static void place_menu(Menu *m, int x, int y)
{
    int widest = 0;

    for (int i = 0; i < m->n; i++)
        if (m->items[i].label && utf8_cols(m->items[i].label) > widest)
            widest = utf8_cols(m->items[i].label);
    m->inner = widest + 4;
    if (m->inner > fm.width - 2)
        m->inner = fm.width - 2 > 1 ? fm.width - 2 : 1;
    m->left = x + m->inner + 1 <= fm.width ? x : fm.width - m->inner - 1;
    m->top = y + m->n + 1 <= fm.height ? y : y - m->n - 1;
    if (m->left < 1)
        m->left = 1;
    if (m->top < 1)
        m->top = 1;
}

static void draw_menu(const Menu *m)
{
    Buf b = { 0 };

    for (int line = 0; line < m->n + 2; line++) {
        const MenuItem *item = line > 0 && line <= m->n ? &m->items[line - 1] : NULL;
        const char *l = line == 0 ? "┌" : line == m->n + 1 ? "└" : item->label ? "│" : "├";
        const char *r = line == 0 ? "┐" : line == m->n + 1 ? "┘" : item->label ? "│" : "┤";

        buf_printf(&b, "\x1b[%d;%dH" BG_MENU COLOR_MENU_BORDER "%s", m->top + line, m->left, l);
        if (item && item->label) {
            int cols = m->inner - 2;
            buf_puts(&b, line - 1 == m->sel ? BG_SELECTED : BG_MENU);
            buf_puts(&b, COLOR_TEXT "  ");
            cols -= buf_cols(&b, item->label, cols);
            while (cols-- > 0)
                buf_add(&b, " ", 1);
            buf_puts(&b, BG_MENU COLOR_MENU_BORDER);
        } else {
            for (int i = 0; i < m->inner; i++)
                buf_puts(&b, "─");
        }
        buf_puts(&b, r);
        buf_puts(&b, RESET);
    }
    if (write(STDOUT_FILENO, b.data, b.len) < 0) {
        /* The terminal is gone. */
    }
    free(b.data);
}

static void menu_step(Menu *m, int step)
{
    int i = m->sel;

    do
        i = (i + step + m->n) % m->n;
    while (m->items[i].action == ACT_SEPARATOR);
    m->sel = i;
}

/* Shows the menu at screen cell (x, y) until an item is picked or it is dismissed. */
static int run_menu(const MenuItem *items, int n, int x, int y)
{
    static const char hover_on[] = "\x1b[?1003h", hover_off[] = "\x1b[?1003l";
    Menu m = { .items = items, .n = n };
    unsigned char buf[512];
    int result = ACT_NONE;
    bool done = false;

    if (write(STDOUT_FILENO, hover_on, sizeof hover_on - 1) < 0)
        return ACT_NONE;
    while (!done) {
        place_menu(&m, x, y);
        draw(NULL, NULL);
        draw_menu(&m);
        int len = read_input(buf, sizeof buf);
        if (len < 0)
            exit(0);
        for (int i = 0; i < len && !done; i++) {
            if (buf[i] == '\r' || buf[i] == ' ') {
                result = items[m.sel].action;
                done = true;
            } else if (buf[i] == 'j' || buf[i] == 'k') {
                menu_step(&m, buf[i] == 'j' ? 1 : -1);
            } else if (buf[i] == 'q' || buf[i] == 0x03) {
                done = true;
            } else if (buf[i] == 0x1B) {
                int k = escape_len(buf + i, len - i);
                const unsigned char *seq = buf + i;
                unsigned char final = seq[k - 1];
                int bt, mx, my;
                i += k - 1;
                if (k == 1) {
                    done = true;
                } else if (k > 3 && seq[2] == '<' &&
                           sscanf((const char *)seq + 3, "%d;%d;%d", &bt, &mx, &my) == 3) {
                    int idx = my - m.top - 1;
                    bool inside = mx > m.left && mx <= m.left + m.inner && idx >= 0 && idx < n;
                    bool usable = inside && items[idx].action != ACT_SEPARATOR;
                    if (bt & 32) { /* motion */
                        if (usable)
                            m.sel = idx;
                    } else if (final == 'M' && (bt == 0 || bt == 2)) {
                        if (usable)
                            result = items[idx].action;
                        if (usable || !inside)
                            done = true;
                    }
                } else if (final == 'A' || final == 'B') {
                    menu_step(&m, final == 'B' ? 1 : -1);
                }
            }
        }
    }
    if (write(STDOUT_FILENO, hover_off, sizeof hover_off - 1) < 0) {
        /* The terminal is gone. */
    }
    return result;
}

static bool is_markdown(const Row *row)
{
    static const char *const extensions[] = { ".md", ".markdown", ".mdown", ".mkd", NULL };
    const char *ext = strrchr(row->name, '.');

    for (int i = 0; !row->dir && ext && extensions[i]; i++)
        if (strcasecmp(ext, extensions[i]) == 0)
            return true;
    return false;
}

static void run_action(int action, bool on_row)
{
    char dir[PATH_MAX];
    Row *row = on_row ? &fm.rows[fm.sel] : NULL;

    switch (action) {
    case ACT_OPEN:
        activate();
        break;
    case ACT_OPEN_SIDE:
        if (row->dir)
            activate();
        else
            send_to_tcode("open-side", row->path);
        break;
    case ACT_PREVIEW:
        send_to_tcode("preview", row->path);
        break;
    case ACT_PROJECT:
        send_to_tcode("project", row->path);
        break;
    case ACT_NEW_FILE:
    case ACT_NEW_FOLDER:
        if (on_row)
            target_dir(dir, sizeof dir);
        else
            snprintf(dir, sizeof dir, "%s", fm.root);
        create_in(dir, action == ACT_NEW_FOLDER);
        break;
    case ACT_TERMINAL:
        snprintf(dir, sizeof dir, "%s", row ? row->path : fm.root);
        if (row && !row->dir)
            *strrchr(dir, '/') = '\0';
        send_to_tcode("terminal", dir);
        break;
    case ACT_COPY_PATH:
        send_to_tcode("copy", row->path);
        break;
    case ACT_COPY_RELATIVE:
        send_to_tcode("copy", relative(row->path));
        break;
    case ACT_RENAME:
        rename_selected();
        break;
    case ACT_DELETE:
        delete_selected();
        break;
    }
}

/* Input -------------------------------------------------------------------------------------- */

static void handle_mouse(int button, int x, int y, bool press)
{
    if (!press)
        return;
    if (button == 64 || button == 65) { /* wheel */
        fm.top += button == 64 ? -3 : 3;
        fm.follow = false;
        return;
    }
    int r = fm.top + (y - 2);
    bool on_row = y >= 2 && r < fm.nrows;
    if (button == 0 && on_row) {
        fm.sel = r;
        fm.follow = true;
        activate();
    } else if (button == 2) { /* right click: context menu */
        if (on_row) {
            fm.sel = r;
            if (fm.rows[r].dir)
                run_action(run_menu(folder_menu, N_ELEMENTS(folder_menu), x, y), true);
            else if (is_markdown(&fm.rows[r]))
                run_action(run_menu(markdown_menu, N_ELEMENTS(markdown_menu), x, y), true);
            else
                run_action(run_menu(row_menu, N_ELEMENTS(row_menu), x, y), true);
        } else {
            run_action(run_menu(empty_menu, N_ELEMENTS(empty_menu), x, y), false);
        }
    }
}

static void handle_csi(const unsigned char *seq, int len)
{
    char params[64] = "";
    unsigned char final = seq[len - 1];
    int plen = len - 3;

    if (plen > 0 && plen < (int)sizeof params)
        memcpy(params, seq + 2, plen);

    if (params[0] == '<' && (final == 'M' || final == 'm')) {
        int b, x, y;
        if (sscanf(params + 1, "%d;%d;%d", &b, &x, &y) == 3)
            handle_mouse(b, x, y, final == 'M');
        return;
    }
    int page = body_height(fm.message[0]) - 1;
    switch (final) {
    case 'A': move(-1); break;
    case 'B': move(1); break;
    case 'C': activate(); break;
    case 'D': collapse(); break;
    case 'H': move(-fm.nrows); break;
    case 'F': move(fm.nrows); break;
    case 'I': fm.focused = true; rebuild(selected_path()); break;
    case 'O': fm.focused = false; break;
    case '~':
        switch (atoi(params)) {
        case 1: case 7: move(-fm.nrows); break;
        case 4: case 8: move(fm.nrows); break;
        case 5: move(-page); break;
        case 6: move(page); break;
        case 3: delete_selected(); break;
        }
        break;
    }
}

static void handle_input(const unsigned char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char c = buf[i];

        if (c != 0x1B || escape_len(buf + i, n - i) == 1)
            fm.message[0] = '\0';
        switch (c) {
        case 0x1B: {
            int len = escape_len(buf + i, n - i);
            if (len >= 3 && buf[i + 1] == '[') {
                handle_csi(buf + i, len);
            } else if (len == 3 && buf[i + 1] == 'O') { /* SS3 arrows, Home, End */
                unsigned char seq[3] = { 0x1B, '[', buf[i + 2] };
                handle_csi(seq, 3);
            }
            i += len - 1;
            break;
        }
        case 'j': move(1); break;
        case 'k': move(-1); break;
        case 'g': move(-fm.nrows); break;
        case 'G': move(fm.nrows); break;
        case '\r': case 'l': activate(); break;
        case 'h': case 0x7F: case 0x08: collapse(); break;
        case 'a': create(); break;
        case 'r': rename_selected(); break;
        case 'd': delete_selected(); break;
        case '.':
            fm.show_hidden = !fm.show_hidden;
            rebuild(selected_path());
            break;
        case 'R': rebuild(selected_path()); break;
        }
    }
}

int main(void)
{
    struct sigaction sa = { .sa_handler = on_winch }; /* no SA_RESTART: read() returns on resize */
    unsigned char buf[512];

    if (!getcwd(fm.root, sizeof fm.root)) {
        perror("t-code-fm");
        return 1;
    }
    sigaction(SIGWINCH, &sa, NULL);
    setup_terminal();
    update_size();
    rebuild(NULL);

    for (;;) {
        draw(NULL, NULL);
        int n = read_input(buf, sizeof buf);
        if (n < 0)
            break;
        handle_input(buf, n);
    }
    return 0;
}
