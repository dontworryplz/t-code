/* Tests for native plugin system in src/plugins/plugins.c */
#include "plugins/plugins.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static char *tmp_dir;
Tab *current_tab(App *t)
{
  (void)t;
  return NULL;
}

void status_say(App *t, const char *message)
{
  (void)t;
  (void)message;
}

static GPtrArray *test_picker_rows = NULL;

void picker_add(App *t, guint generation, const char *label, const char *detail, const char *payload)
{
  (void)t;
  (void)generation;
  if (!test_picker_rows)
    test_picker_rows = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(test_picker_rows, g_strdup_printf("%s|%s|%s", label, detail ? detail : "", payload));
}

int fuzzy_score(const char *query, const char *text)
{
  const char *base = strrchr(text, '/');
  const char *p = text;
  int total = 0, prev = -2;

  base = base ? base + 1 : text;
  for (const char *q = query; *q; q++) {
    char c = g_ascii_tolower(*q);
    if (c == ' ')
      continue;
    while (*p && g_ascii_tolower(*p) != c)
      p++;
    if (!*p)
      return -1;
    int at = p - text;
    total += 1;
    if (at == prev + 1)
      total += 5;
    if (at == 0 || strchr("/_-. :", text[at - 1]))
      total += 8;
    if (p >= base)
      total += 3;
    prev = at;
    p++;
  }
  return total * 16 - (int)strlen(text);
}


static void write_file(const char *rel_path, const char *content, int mode)
{
  char *full = g_build_filename(tmp_dir, rel_path, NULL);
  char *dir = g_path_get_dirname(full);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  g_file_set_contents(full, content, -1, NULL);
  if (mode != 0)
    g_chmod(full, mode);
  g_free(full);
}

static void test_empty_dir(void)
{
  App app = { 0 };
  char *empty = g_build_filename(tmp_dir, "empty", NULL);
  g_mkdir_with_parents(empty, 0755);

  app.plugins = g_ptr_array_new();
  plugins_scan_dir(&app, empty);
  g_assert_cmpuint(app.plugins->len, ==, 0);

  plugins_stop(&app);
  g_free(empty);
}

static void test_valid_plugin(void)
{
  App app = { 0 };
  const char *json = "{\n"
                     "  \"name\": \"Test Plugin\",\n"
                     "  \"version\": \"1.2.3\",\n"
                     "  \"description\": \"A sample test plugin\",\n"
                     "  \"executable\": \"run.sh\",\n"
                     "  \"commands\": [\n"
                     "    { \"id\": \"c1\", \"name\": \"Command One\", \"keys\": \"Ctrl+Alt+1\" },\n"
                     "    { \"id\": \"c2\", \"name\": \"Command Two\" }\n"
                     "  ]\n"
                     "}\n";

  write_file("valid/test-plug/plugin.json", json, 0644);
  write_file("valid/test-plug/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  char *scan_path = g_build_filename(tmp_dir, "valid", NULL);
  app.plugins = g_ptr_array_new();
  plugins_scan_dir(&app, scan_path);

  g_assert_cmpuint(app.plugins->len, ==, 1);
  Plugin *p = g_ptr_array_index(app.plugins, 0);
  g_assert_cmpstr(p->id, ==, "test-plug");
  g_assert_cmpstr(p->name, ==, "Test Plugin");
  g_assert_cmpstr(p->version, ==, "1.2.3");
  g_assert_cmpstr(p->description, ==, "A sample test plugin");
  g_assert_cmpuint(p->commands->len, ==, 2);

  PluginCommand *c1 = g_ptr_array_index(p->commands, 0);
  g_assert_cmpstr(c1->id, ==, "c1");
  g_assert_cmpstr(c1->name, ==, "Command One");
  g_assert_cmpstr(c1->keys, ==, "Ctrl+Alt+1");
  g_assert_true(c1->plugin == p);

  PluginCommand *c2 = g_ptr_array_index(p->commands, 1);
  g_assert_cmpstr(c2->id, ==, "c2");
  g_assert_cmpstr(c2->name, ==, "Command Two");
  g_assert_null(c2->keys);

  plugins_stop(&app);
  g_free(scan_path);
}

static void test_invalid_plugins(void)
{
  App app = { 0 };

  /* Missing name */
  write_file("invalid/p1/plugin.json", "{\"commands\": [{\"id\":\"a\",\"name\":\"A\"}], \"executable\":\"run.sh\"}",
             0644);
  write_file("invalid/p1/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Missing commands */
  write_file("invalid/p2/plugin.json", "{\"name\": \"P2\", \"executable\":\"run.sh\"}", 0644);
  write_file("invalid/p2/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Empty commands array */
  write_file("invalid/p3/plugin.json", "{\"name\": \"P3\", \"commands\": [], \"executable\":\"run.sh\"}", 0644);
  write_file("invalid/p3/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Malformed JSON */
  write_file("invalid/p4/plugin.json", "NOT JSON", 0644);
  write_file("invalid/p4/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Non-existent executable */
  write_file("invalid/p5/plugin.json",
             "{\"name\": \"P5\", \"commands\": [{\"id\":\"a\",\"name\":\"A\"}], \"executable\":\"missing.sh\"}", 0644);

  /* Non-executable file */
  write_file("invalid/p6/plugin.json",
             "{\"name\": \"P6\", \"commands\": [{\"id\":\"a\",\"name\":\"A\"}], \"executable\":\"run.sh\"}", 0644);
  write_file("invalid/p6/run.sh", "#!/bin/sh\nexit 0\n", 0644); /* Not executable */

  char *scan_path = g_build_filename(tmp_dir, "invalid", NULL);
  app.plugins = g_ptr_array_new();
  plugins_scan_dir(&app, scan_path);

  g_assert_cmpuint(app.plugins->len, ==, 0);

  plugins_stop(&app);
  g_free(scan_path);
}

static void test_subprocess_execution(void)
{
  App app = { 0 };
  char *out_path = g_build_filename(tmp_dir, "exec_output.txt", NULL);
  g_unlink(out_path);

  char *script = g_strdup_printf("#!/bin/sh\n"
                                 "echo \"SOCKET=$TCODE_SOCKET\" > \"%s\"\n"
                                 "echo \"PLUGIN=$TCODE_PLUGIN_ID\" >> \"%s\"\n"
                                 "echo \"CMD=$TCODE_COMMAND_ID\" >> \"%s\"\n"
                                 "echo \"ROOT=$TCODE_PROJECT_ROOT\" >> \"%s\"\n"
                                 "exit 0\n",
                                 out_path, out_path, out_path, out_path);

  write_file("exec/runner/plugin.json",
             "{\"name\": \"Runner\", \"commands\": [{\"id\":\"go\",\"name\":\"Run Go\"}], \"executable\":\"run.sh\"}",
             0644);
  write_file("exec/runner/run.sh", script, 0755);
  g_free(script);

  app.paths.socket = g_strdup("/tmp/test-tcode.sock");
  app.project.root = g_strdup("/home/user/project");

  char *scan_path = g_build_filename(tmp_dir, "exec", NULL);
  app.plugins = g_ptr_array_new();
  plugins_scan_dir(&app, scan_path);
  g_assert_cmpuint(app.plugins->len, ==, 1);

  plugin_run_command(&app, "runner", "go");

  /* Spin main context until process finishes and output file is created */
  int iters = 0;
  while (!g_file_test(out_path, G_FILE_TEST_EXISTS) && iters < 100) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(20000); /* 20ms */
    iters++;
  }

  g_assert_true(g_file_test(out_path, G_FILE_TEST_EXISTS));

  char *contents = NULL;
  g_file_get_contents(out_path, &contents, NULL, NULL);
  g_assert_nonnull(contents);
  g_assert_nonnull(strstr(contents, "SOCKET=/tmp/test-tcode.sock"));
  g_assert_nonnull(strstr(contents, "PLUGIN=runner"));
  g_assert_nonnull(strstr(contents, "CMD=go"));
  g_assert_nonnull(strstr(contents, "ROOT=/home/user/project"));

  g_free(contents);
  g_free(out_path);
  g_free(scan_path);
  g_free(app.paths.socket);
  g_free(app.project.root);
  plugins_stop(&app);
}
static void test_palette_query(void)
{
  App app = { 0 };
  char *scan_path = g_build_filename(tmp_dir, "valid", NULL);
  app.plugins = g_ptr_array_new();
  plugins_scan_dir(&app, scan_path);

  if (test_picker_rows) {
    g_ptr_array_free(test_picker_rows, TRUE);
    test_picker_rows = NULL;
  }

  int matches = plugins_palette_query(&app, "Command", 1);
  g_assert_cmpint(matches, ==, 2);
  g_assert_nonnull(test_picker_rows);
  g_assert_cmpuint(test_picker_rows->len, ==, 2);

  gboolean handled = plugins_palette_choose(&app, "plugin:test-plug:c1");
  g_assert_true(handled);

  gboolean unhandled = plugins_palette_choose(&app, "nonexistent");
  g_assert_false(unhandled);

  if (test_picker_rows) {
    g_ptr_array_free(test_picker_rows, TRUE);
    test_picker_rows = NULL;
  }

  plugins_stop(&app);
  g_free(scan_path);
}


int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  tmp_dir = g_dir_make_tmp("t-code-test-plugins-XXXXXX", NULL);

  g_test_add_func("/plugins/empty-dir", test_empty_dir);
  g_test_add_func("/plugins/valid-plugin", test_valid_plugin);
  g_test_add_func("/plugins/invalid-plugins", test_invalid_plugins);
  g_test_add_func("/plugins/subprocess-execution", test_subprocess_execution);
  g_test_add_func("/plugins/palette-query", test_palette_query);

  int status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp_dir, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp_dir);
  return status;
}
