/* File operations: opening, creating, renaming, deleting, cut / copy / paste and moving. */
#include "fm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void send_to_tcode(const char *command, const char *arg)
{
  const char *socket_path = getenv("TCODE_SOCKET");
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  int fd;

  if (!socket_path || strlen(socket_path) >= sizeof addr.sun_path) {
    error_message("TCODE_SOCKET is not set");
    return;
  }
  strcpy(addr.sun_path, socket_path);
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    error_message("cannot reach t-code: %s", strerror(errno));
    if (fd >= 0)
      close(fd);
    return;
  }
  if (dprintf(fd, "%s %s\n", command, arg) < 0)
    error_message("cannot reach t-code: %s", strerror(errno));
  close(fd);
}

bool is_markdown(const Row *row)
{
  static const char *const extensions[] = { ".md", ".markdown", ".mdown", ".mkd", NULL };
  const char *ext = strrchr(row->name, '.');

  for (int i = 0; !row->dir && ext && extensions[i]; i++)
    if (strcasecmp(ext, extensions[i]) == 0)
      return true;
  return false;
}

/* Opening and folding ---------------------------------------------------------------------------- */

void activate(void)
{
  if (!fm.nrows)
    return;
  Row *row = &fm.rows[fm.sel];
  if (row->dir) {
    set_expanded(row->path, is_expanded(row->path) < 0);
    rebuild_rows(row->path);
  } else {
    send_to_tcode("open", row->path);
  }
}

/* Collapses the selected folder, or else its parent (and moves there). */
void collapse(void)
{
  if (!fm.nrows)
    return;
  Row *row = &fm.rows[fm.sel];
  if (row->dir && is_expanded(row->path) >= 0) {
    set_expanded(row->path, false);
    rebuild_rows(row->path);
    return;
  }
  char parent[PATH_MAX];
  parent_dir(row->path, parent, sizeof parent);
  if (strcmp(parent, fm.root) != 0) {
    set_expanded(parent, false);
    rebuild_rows(parent);
  }
}

/* The folder new files go into: the selected folder, or the selected file's folder. */
void target_dir(char *out, size_t cap)
{
  if (!fm.nrows) {
    snprintf(out, cap, "%s", fm.root);
    return;
  }
  Row *row = &fm.rows[fm.sel];
  if (row->dir) {
    snprintf(out, cap, "%s", row->path);
    set_expanded(row->path, true);
  } else {
    parent_dir(row->path, out, cap);
  }
}

/* Creating, renaming, deleting ------------------------------------------------------------------- */

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
void create_in(const char *dir, bool folder)
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
      error_message("cannot create %s: %s", name, strerror(errno));
  } else {
    char *slash = strrchr(path, '/');
    *slash = '\0';
    int rc = make_dirs(path);
    *slash = '/';
    int fd = rc < 0 ? -1 : open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
      error_message("cannot create %s: %s", name, strerror(errno));
    else
      close(fd);
  }
  reveal(path);
  rebuild(path);
}

void create(void)
{
  char dir[PATH_MAX];

  target_dir(dir, sizeof dir);
  create_in(dir, false);
}

void rename_selected(void)
{
  char name[NAME_MAX * 4], dest[PATH_MAX * 2];

  if (!fm.nrows)
    return;
  Row *row = &fm.rows[fm.sel];
  if (!prompt("Rename to: ", name, sizeof name, row->name) || !name[0] || strchr(name, '/'))
    return;

  snprintf(dest, sizeof dest, "%.*s/%s", (int)(row->name - 1 - row->path), row->path, name);
  if (access(dest, F_OK) == 0) {
    error_message("%s already exists", name);
    return;
  }
  if (rename(row->path, dest) < 0) {
    error_message("cannot rename: %s", strerror(errno));
    return;
  }
  rebuild(dest);
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
  (void)st, (void)type, (void)ftw;
  return remove(path);
}

static int remove_tree(const char *path)
{
  return nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

void delete_selected(void)
{
  char question[NAME_MAX * 4 + 64];

  if (!fm.nrows)
    return;
  Row *row = &fm.rows[fm.sel];
  snprintf(question, sizeof question, "Delete %s%s? (y/n) ", row->name, row->dir ? "/ and everything in it" : "");
  if (!confirm(question))
    return;
  if (remove_tree(row->path) < 0)
    error_message("cannot delete %s: %s", row->name, strerror(errno));
  rebuild(NULL);
}

/* Copying and moving ----------------------------------------------------------------------------- */

static int copy_file(const char *src, const char *dst, mode_t mode)
{
  char buf[65536];
  int in = open(src, O_RDONLY | O_CLOEXEC), out;
  ssize_t n;

  if (in < 0)
    return -1;
  out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  if (out < 0) {
    close(in);
    return -1;
  }
  while ((n = read(in, buf, sizeof buf)) > 0) {
    for (ssize_t done = 0; done < n;) {
      ssize_t w = write(out, buf + done, n - done);
      if (w < 0) {
        close(in);
        close(out);
        unlink(dst);
        return -1;
      }
      done += w;
    }
  }
  close(in);
  if (close(out) < 0 || n < 0) {
    unlink(dst);
    return -1;
  }
  return 0;
}

/* Copies a file, a symlink, or a folder with everything in it. */
static int copy_tree(const char *src, const char *dst)
{
  struct stat st;

  if (lstat(src, &st) < 0)
    return -1;
  if (S_ISLNK(st.st_mode)) {
    char target[PATH_MAX];
    ssize_t n = readlink(src, target, sizeof target - 1);
    if (n < 0)
      return -1;
    target[n] = '\0';
    return symlink(target, dst);
  }
  if (!S_ISDIR(st.st_mode))
    return copy_file(src, dst, st.st_mode & 07777);

  DIR *d;
  struct dirent *e;
  int rc = 0;
  if (mkdir(dst, st.st_mode & 07777) < 0 || !(d = opendir(src)))
    return -1;
  while (rc == 0 && (e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    char *from = join(src, e->d_name), *to = join(dst, e->d_name);
    rc = copy_tree(from, to);
    free(from);
    free(to);
  }
  closedir(d);
  return rc;
}

static bool exists(const char *path)
{
  struct stat st;

  return lstat(path, &st) == 0;
}

/* dir/name, or like VS Code "name copy.ext", "name copy 2.ext", ... when that is taken. */
static void free_name(const char *dir, const char *name, bool is_dir, char *out, size_t cap)
{
  const char *dot = is_dir ? NULL : strrchr(name, '.');
  int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
  const char *ext = dot && dot != name ? dot : "";

  snprintf(out, cap, "%s/%s", dir, name);
  for (int i = 1; exists(out); i++) {
    if (i == 1)
      snprintf(out, cap, "%s/%.*s copy%s", dir, stem, name, ext);
    else
      snprintf(out, cap, "%s/%.*s copy %d%s", dir, stem, name, i, ext);
  }
}

static bool inside(const char *path, const char *folder)
{
  size_t n = strlen(folder);

  return strcmp(path, folder) == 0 || (strncmp(path, folder, n) == 0 && path[n] == '/');
}

/* Copies or moves src into dir; returns false (after saying why) when it did not. */
static bool put(const char *src, const char *dir, bool cut, char *dest, size_t cap)
{
  struct stat st;
  char parent[PATH_MAX];

  if (lstat(src, &st) < 0) {
    error_message("%s is gone", src);
    return false;
  }
  parent_dir(src, parent, sizeof parent);
  if (cut && strcmp(parent, dir) == 0)
    return false; /* already there */
  if (S_ISDIR(st.st_mode) && inside(dir, src)) {
    error_message("cannot put a folder inside itself");
    return false;
  }
  free_name(dir, strrchr(src, '/') + 1, S_ISDIR(st.st_mode), dest, cap);
  if (cut && rename(src, dest) == 0)
    return true;
  if (cut && errno != EXDEV) {
    error_message("cannot move: %s", strerror(errno));
    return false;
  }
  /* A copy, or a move to another file system: copy, then remove the original. */
  if (copy_tree(src, dest) < 0) {
    error_message("cannot copy: %s", strerror(errno));
    return false;
  }
  if (cut && remove_tree(src) < 0)
    error_message("copied, but cannot remove the original: %s", strerror(errno));
  return true;
}

void clip_selected(bool cut)
{
  if (!fm.nrows)
    return;
  snprintf(fm.clip, sizeof fm.clip, "%s", fm.rows[fm.sel].path);
  fm.clip_cut = cut;
  message("%s %s, Ctrl+V pastes it", cut ? "Cut" : "Copied", fm.rows[fm.sel].name);
}

void paste_into(const char *dir)
{
  char dest[PATH_MAX * 2], parent[PATH_MAX];

  if (!fm.clip[0]) {
    message("Nothing to paste: Ctrl+C or Ctrl+X a file first");
    return;
  }
  if (!fm.clip_cut && strcmp(fm.clip, dir) == 0) {
    /* A copied folder pasted while it is still selected: the copy goes beside it ("foo copy"). */
    parent_dir(dir, parent, sizeof parent);
    dir = parent;
  }
  if (put(fm.clip, dir, fm.clip_cut, dest, sizeof dest)) {
    if (fm.clip_cut)
      fm.clip[0] = '\0';
    set_expanded(dir, true);
    reveal(dest);
    rebuild(dest);
  } else {
    rebuild(selected_path());
  }
}

/* Drag and drop. */
void move_into(const char *src, const char *dir)
{
  char dest[PATH_MAX * 2];

  if (put(src, dir, true, dest, sizeof dest)) {
    if (strcmp(fm.clip, src) == 0)
      fm.clip[0] = '\0';
    message("Moved %s to %s/", strrchr(dest, '/') + 1, *relative(dir) ? relative(dir) : ".");
    set_expanded(dir, true);
    reveal(dest);
    rebuild(dest);
  }
}
