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

static bool ignored_name(const char *name, size_t len)
{
  static const char *const names[] = { "build", "target", "node_modules", ".git", ".cache" };

  for (size_t i = 0; i < sizeof names / sizeof *names; i++)
    if (strlen(names[i]) == len && strncmp(name, names[i], len) == 0)
      return true;
  return false;
}

/* Build and cache folders churn during compilation: never watch inside them. */
static bool ignored_dir(const char *path)
{
  size_t root_len = strlen(fm.root);
  const char *p = strncmp(path, fm.root, root_len) == 0 ? path + root_len : path;

  while (*p == '/')
    p++;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (ignored_name(p, len))
      return true;
    p += len;
    while (*p == '/')
      p++;
  }
  return false;
}

static bool wanted(const char *path, bool *git)
{
  *git = fm.git_dir[0] && strcmp(path, fm.git_dir) == 0;
  if (*git)
    return true;
  if (strcmp(path, fm.root) == 0)
    return true;
  if (ignored_dir(path))
    return false;
  return is_expanded(path) >= 0;
}

static void add(const char *path, bool git)
{
  if (!git && strcmp(path, fm.root) != 0 && ignored_dir(path))
    return;
  for (int i = 0; i < nwatches; i++)
    if (strcmp(watches[i].path, path) == 0)
      return;
  int wd = inotify_add_watch(fd, path, git ? GIT_EVENTS : DIR_EVENTS);
  if (wd < 0)
    return;
  watches = realloc(watches, (nwatches + 1) * sizeof *watches);
  watches[nwatches++] = (Watch){ wd, strdup(path), git };
}

/* Watches the root, the open folders (except build/cache trees) and the .git folder. */
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
      else if (!w->git) {
        if (e->len && ignored_name(e->name, strlen(e->name)) &&
            !(e->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)))
          continue;
        changed = true;
      } else if (e->len && (strcmp(e->name, "index") == 0 || strcmp(e->name, "HEAD") == 0))
        changed = true; /* git add, commit, checkout, ... (not its lock and temp files) */
    }
  }
  return changed;
}
