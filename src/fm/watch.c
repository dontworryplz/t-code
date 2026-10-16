/*
 * Refreshing on changes: inotify watches the root, every open folder, and the repository's .git
 * folder (its index and HEAD change on add, commit, checkout), so new, deleted, saved and
 * committed files show up by themselves.
 */
#include "fm.h"

#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define DIR_EVENTS (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ONLYDIR)
#define GIT_EVENTS (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ONLYDIR)

typedef struct {
    int wd;
    char *path;
    bool git;
} Watch;

static int fd = -1;
static Watch *watches;
static int nwatches;

int watch_fd(void)
{
    if (fd < 0)
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    return fd;
}

static bool wanted(const char *path, bool *git)
{
    *git = fm.git_dir[0] && strcmp(path, fm.git_dir) == 0;
    if (*git || strcmp(path, fm.root) == 0)
        return true;
    return is_expanded(path) >= 0;
}

static void add(const char *path, bool git)
{
    for (int i = 0; i < nwatches; i++)
        if (strcmp(watches[i].path, path) == 0)
            return;
    int wd = inotify_add_watch(fd, path, git ? GIT_EVENTS : DIR_EVENTS);
    if (wd < 0)
        return;
    watches = realloc(watches, (nwatches + 1) * sizeof *watches);
    watches[nwatches++] = (Watch){ wd, strdup(path), git };
}

/* Watches exactly the root, the open folders and the .git folder. */
void watch_sync(void)
{
    bool git;

    if (watch_fd() < 0)
        return;
    for (int i = 0; i < nwatches;) {
        if (!wanted(watches[i].path, &git)) {
            inotify_rm_watch(fd, watches[i].wd);
            free(watches[i].path);
            watches[i] = watches[--nwatches];
        } else {
            i++;
        }
    }
    add(fm.root, false);
    for (int i = 0; i < fm.nexpanded; i++)
        add(fm.expanded[i], false);
    if (fm.git_dir[0])
        add(fm.git_dir, true);
}

static const Watch *find(int wd)
{
    for (int i = 0; i < nwatches; i++)
        if (watches[i].wd == wd)
            return &watches[i];
    return NULL;
}

/* Reads what inotify has; true when something worth a refresh happened. */
bool watch_read(void)
{
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t n;

    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            const Watch *w = find(e->wd);
            p += sizeof *e + e->len;

            if (e->mask & IN_Q_OVERFLOW)
                changed = true;
            else if (!w || (e->mask & IN_IGNORED))
                continue;
            else if (!w->git)
                changed = true;
            else if (e->len && (strcmp(e->name, "index") == 0 || strcmp(e->name, "HEAD") == 0))
                changed = true; /* git add, commit, checkout, ... (not its lock and temp files) */
        }
    }
    return changed;
}
