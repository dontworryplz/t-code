/*
 * Tests that show a window run it on GTK's Broadway backend, so nothing appears on the screen:
 * start_broadway() starts a gtk4-broadwayd of their own (without one, the window opens on the
 * desktop for a moment), stop_broadway() ends it. Included by the tests that need it.
 */
#pragma once

#include <glib.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static GPid broadway;

/* gtk4-broadwayd ends with the test, also when an assertion aborts it. */
static void broadway_child_setup(gpointer data G_GNUC_UNUSED)
{
  prctl(PR_SET_PDEATHSIG, SIGTERM);
}

/* Whether display n looks taken: its socket is there (another gtk4-broadwayd, maybe one a crashed
 * test left). Starting one there would take that socket over and then fail on its port. */
static gboolean broadway_taken(int n)
{
  char *name = g_strdup_printf("broadway%d.socket", n + 1);
  char *path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
  gboolean taken = g_file_test(path, G_FILE_TEST_EXISTS);

  g_free(path);
  g_free(name);
  return taken;
}

static void start_broadway(void)
{
  for (int tries = 0; tries < 20 && !broadway; tries++) {
    int n = 50 + g_random_int_range(0, 50), status;
    char *display = g_strdup_printf(":%d", n);
    char *argv[] = { "gtk4-broadwayd", display, NULL };
    GPid pid;

    if (!broadway_taken(n) && g_spawn_async(NULL, argv, NULL,
                                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                            broadway_child_setup, NULL, &pid, NULL)) {
      g_usleep(300 * 1000);                      /* until it listens */
      if (waitpid(pid, &status, WNOHANG) == 0) { /* still running: it got the display */
        broadway = pid;
        g_setenv("GDK_BACKEND", "broadway", TRUE);
        g_setenv("BROADWAY_DISPLAY", display, TRUE);
      }
    }
    g_free(display);
  }
}

static void stop_broadway(void)
{
  if (broadway) {
    kill(broadway, SIGTERM);
    waitpid(broadway, NULL, 0);
    broadway = 0;
  }
}
