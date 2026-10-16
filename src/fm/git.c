/* Git state of the files, shown in VS Code's colors with its letter at the end of the row. */
#include "fm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* VS Code's git decoration colors. */
#define COLOR_GIT_MODIFIED "\x1b[38;2;226;192;141m"
#define COLOR_GIT_ADDED "\x1b[38;2;129;184;139m"
#define COLOR_GIT_UNTRACKED "\x1b[38;2;115;201;145m"
#define COLOR_GIT_DELETED "\x1b[38;2;199;78;57m"
#define COLOR_GIT_CONFLICT "\x1b[38;2;228;103;107m"
#define COLOR_GIT_IGNORED "\x1b[38;2;140;140;140m"

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
void git_refresh(void)
{
    char *where_argv[] = { "git", "-C", fm.root, "rev-parse", "--show-toplevel", "--absolute-git-dir", NULL };
    char *status_argv[] = { "git", "-C", fm.root, "status", "--porcelain=v1", "-z",
                            "--untracked-files=all", "--ignored=matching", NULL };
    size_t where_len, len;
    char *where, *out;

    for (int i = 0; i < fm.ngit; i++)
        free(fm.git[i].path);
    fm.ngit = 0;
    fm.git_dir[0] = '\0';

    where = run_capture(where_argv, &where_len); /* "<top>\n<git dir>\n" */
    if (!where)
        return;
    char *git_dir = strchr(where, '\n');
    if (git_dir) {
        *git_dir++ = '\0';
        git_dir[strcspn(git_dir, "\n")] = '\0';
        snprintf(fm.git_dir, sizeof fm.git_dir, "%s", git_dir);
    }
    out = run_capture(status_argv, &len);
    for (size_t i = 0; out && i + 3 < len;) {
        char *entry = out + i;
        char x = entry[0], y = entry[1];
        i += strlen(entry) + 1;
        if (x == 'R' || x == 'C') /* the original name of a rename or copy follows */
            i += strlen(out + i) + 1;

        char *path = join(where, entry + 3);
        size_t plen = strlen(path);
        if (plen > 1 && path[plen - 1] == '/')
            path[plen - 1] = '\0';
        fm.git = realloc(fm.git, (fm.ngit + 1) * sizeof *fm.git);
        fm.git[fm.ngit++] = (GitEntry){ path, git_state(x, y) };
    }
    free(out);
    free(where);
}

/* The state shown for a row: its own, or for a folder the strongest among what is inside;
 * anything inside an ignored folder is ignored too. */
char git_state_of(const char *path, bool dir)
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

const char *git_color(char state)
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
const char *git_badge(char state)
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
