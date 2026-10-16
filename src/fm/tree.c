/* The folder tree: which folders are open, reading them, and the selection. */
#include "fm.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    char *path = malloc(n + strlen(name) + 2);

    if (!path)
        abort();
    sprintf(path, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", name);
    return path;
}

int is_expanded(const char *path)
{
    for (int i = 0; i < fm.nexpanded; i++)
        if (strcmp(fm.expanded[i], path) == 0)
            return i;
    return -1;
}

void set_expanded(const char *path, bool on)
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

/* Like VS Code: dotfiles show, version control folders never do; "." hides every dotfile. */
static bool shown(const char *name)
{
    static const char *const never[] = { ".git", ".svn", ".hg", NULL };

    if (name[0] != '.')
        return true;
    if (!fm.show_hidden)
        return false;
    for (int i = 0; never[i]; i++)
        if (strcmp(name, never[i]) == 0)
            return false;
    return true;
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
        if (!shown(e->d_name))
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

/* Re-reads the tree and its git state, and puts the selection on `keep` when it is still there. */
void rebuild(const char *keep)
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
    watch_sync();
}

/* Marks every directory between the root and path as expanded. */
void reveal(const char *path)
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

const char *selected_path(void)
{
    return fm.nrows ? fm.rows[fm.sel].path : NULL;
}

const char *relative(const char *path)
{
    size_t n = strlen(fm.root);

    if (strncmp(path, fm.root, n) == 0 && path[n] == '/')
        return path + n + 1;
    return strcmp(path, fm.root) == 0 ? "" : path;
}

void move(int delta)
{
    fm.sel += delta;
    if (fm.sel >= fm.nrows)
        fm.sel = fm.nrows - 1;
    if (fm.sel < 0)
        fm.sel = 0;
    fm.follow = true;
}

/* Rows start on screen line 2, under the root's name. */
int row_at(int y)
{
    int r = fm.top + (y - 2);

    return y >= 2 && r < fm.nrows ? r : -1;
}
