/*
 * The control pipe to micro's plugin (see control.h).
 */
#define _DEFAULT_SOURCE /* O_CLOEXEC under -std=c17 */
#include "editor/control.h"

#include "editor/editorview.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int control_open(int number, char **path)
{
    int fd = -1;

    /* O_RDWR keeps the pipe open with no reader yet, and lets writes wait in it until micro reads. */
    *path = g_strdup_printf("%s/t-code-%d-editor-%d", g_get_user_runtime_dir(), getpid(), number);
    unlink(*path);
    if (mkfifo(*path, 0600) == 0)
        fd = open(*path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_warning("t-code: no control pipe %s: %s", *path, g_strerror(errno));
        unlink(*path);
        g_clear_pointer(path, g_free);
    }
    return fd;
}

void control_close(int fd, char *path)
{
    if (fd >= 0)
        close(fd);
    if (path) {
        unlink(path);
        g_free(path);
    }
}

gboolean editor_control(Tab *tab, const char *line)
{
    EditorView *v = tab->view;
    char *data;
    gboolean ok;

    if (!v || v->control_fd < 0)
        return FALSE;
    data = g_strdup_printf("%s\n", line);
    ok = write(v->control_fd, data, strlen(data)) == (ssize_t)strlen(data);
    g_free(data);
    return ok;
}
