/*
 * Every command of the window in one table: the keyboard shortcuts, the command palette
 * (Ctrl+Shift+P) and the Keybinds list are all made from it.
 */
#include "app.h"

#include <stdlib.h>

typedef struct {
    const char *name;
    const char *trigger; /* GTK shortcut trigger, NULL when there is none */
    const char *keys;    /* the shortcut as shown */
    void (*run)(App *t);
} Command;

/* What the commands do --------------------------------------------------------------------- */

static void build(App *t) { build_project(t, FALSE); }
static void run(App *t) { build_project(t, TRUE); }
static void next_tab(App *t) { step_tab(t, 1); }
static void prev_tab(App *t) { step_tab(t, -1); }
static void focus_left(App *t) { focus_group(t, 0); }
static void focus_right(App *t) { focus_group(t, 1); }
static void zoom_in(App *t) { set_font_scale(t, t->font_scale * 1.1); }
static void zoom_out(App *t) { set_font_scale(t, t->font_scale / 1.1); }
static void zoom_reset(App *t) { set_font_scale(t, 1.0); }
static void toggle_files(App *t) { toggle_pane(t, GTK_WIDGET(t->fm)); }
static void toggle_shell(App *t) { toggle_pane(t, GTK_WIDGET(t->shell)); }

static void focus_fm(App *t)
{
    set_pane_visible(t, GTK_WIDGET(t->fm), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(t->fm));
}

static void focus_shell(App *t)
{
    set_pane_visible(t, GTK_WIDGET(t->shell), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(t->shell));
}

static void toggle_split(App *t)
{
    set_split(t, !gtk_widget_get_visible(GTK_WIDGET(t->groups[1])));
    focus_current_tab(t);
}

/* Opens micro's command bar with "goto " typed; the line number and Enter follow. */
static void go_to_line(App *t)
{
    Tab *tab = current_tab(t);

    if (tab && tab->kind == TAB_EDITOR) {
        select_tab(tab);
        feed_editor(tab, "\x05goto ");
    }
}

static void preview(App *t)
{
    Tab *tab = current_tab(t);

    if (tab && tab->kind == TAB_EDITOR && is_markdown(tab->path))
        open_preview(t, tab->path);
}

static void compare_head(App *t)
{
    const char *path = comparable_path(t);

    if (path)
        compare_with_head(t, path);
}

static void history(App *t)
{
    const char *path = comparable_path(t);

    if (path)
        file_history(t, path);
}

/* The table -------------------------------------------------------------------------------------- */

static const Command commands[] = {
    { "Go to File…", "<Control>p", "Ctrl+P", quick_open_show },
    { "Search in Files…", "<Control><Shift>f", "Ctrl+Shift+F", search_show },
    { "Command Palette…", "<Control><Shift>p|F1", "Ctrl+Shift+P, F1", command_palette },
    { "Go to Line…", "<Control>g", "Ctrl+G", go_to_line },
    { "Build", "<Control><Shift>b", "Ctrl+Shift+B", build },
    { "Build and Run", "F5", "F5", run },
    { "Git: Compare with HEAD", "<Control><Alt>d", "Ctrl+Alt+D", compare_head },
    { "Git: File History…", "<Control><Alt>h", "Ctrl+Alt+H", history },
    { "Git: Stop Comparing", NULL, NULL, stop_comparing },
    { "Markdown Preview", "<Control><Shift>v", "Ctrl+Shift+V", preview },
    { "Split Editor", "<Control>backslash", "Ctrl+\\", toggle_split },
    { "Focus Left Editor Group", "<Control>1", "Ctrl+1", focus_left },
    { "Focus Right Editor Group", "<Control>2", "Ctrl+2", focus_right },
    { "Next Tab", "<Control>Page_Down|<Alt>period", "Ctrl+PageDown, Alt+.", next_tab },
    { "Previous Tab", "<Control>Page_Up|<Alt>comma", "Ctrl+PageUp, Alt+,", prev_tab },
    { "Focus File Manager", "<Alt>1", "Alt+1", focus_fm },
    { "Focus Editor", "<Alt>2", "Alt+2", focus_current_tab },
    { "Focus Shell", "<Alt>3", "Alt+3", focus_shell },
    { "Toggle File Manager", "<Control>b", "Ctrl+B", toggle_files },
    { "Toggle Shell", "<Control>j", "Ctrl+J", toggle_shell },
    { "Toggle Code Coloring", "F7", "F7", toggle_syntax },
    { "Zoom In", "<Control>equal|<Control>plus|<Control>KP_Add", "Ctrl+=", zoom_in },
    { "Zoom Out", "<Control>minus|<Control>KP_Subtract", "Ctrl+-", zoom_out },
    { "Reset Zoom", "<Control>0|<Control>KP_0", "Ctrl+0", zoom_reset },
    { "Open Folder…", "<Control><Shift>o", "Ctrl+Shift+O", choose_folder },
    { "Settings…", "<Control>comma", "Ctrl+,", show_settings },
    { "Keyboard Shortcuts", NULL, NULL, show_keybinds },
};

const KeyHelp *window_keys(int *n)
{
    static KeyHelp keys[G_N_ELEMENTS(commands)];

    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
        keys[i] = (KeyHelp){ commands[i].name, commands[i].keys };
    *n = G_N_ELEMENTS(commands);
    return keys;
}

/* Shortcuts ---------------------------------------------------------------------------------- */

static gboolean on_shortcut(GtkWidget *window, GVariant *args G_GNUC_UNUSED, gpointer data)
{
    const Command *command = data;

    command->run(g_object_get_data(G_OBJECT(window), "t-code-app"));
    return TRUE;
}

void add_shortcuts(App *t)
{
    GtkEventController *controller = gtk_shortcut_controller_new();

    g_object_set_data(G_OBJECT(t->window), "t-code-app", t);
    /* Capture phase: the window sees these keys before the focused terminal does. */
    gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
        if (!commands[i].trigger)
            continue;
        gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(controller),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string(commands[i].trigger),
                             gtk_callback_action_new(on_shortcut, (gpointer)&commands[i], NULL)));
    }
    gtk_widget_add_controller(GTK_WIDGET(t->window), controller);
}

/* Command palette ------------------------------------------------------------------------------ */

typedef struct {
    int index, score;
} Match;

static int by_score(gconstpointer a, gconstpointer b)
{
    const Match *x = a, *y = b;

    return x->score != y->score ? y->score - x->score : x->index - y->index;
}

/* The table order when nothing is typed, otherwise the best match first. */
static void query(App *t, const char *text, guint generation)
{
    Match matches[G_N_ELEMENTS(commands)];
    int n = 0;

    for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
        int score = *text ? fuzzy_score(text, commands[i].name) : 0;
        if (score >= 0)
            matches[n++] = (Match){ (int)i, score };
    }
    qsort(matches, n, sizeof *matches, (int (*)(const void *, const void *))by_score);
    for (int i = 0; i < n; i++) {
        const Command *c = &commands[matches[i].index];
        char *index = g_strdup_printf("%d", matches[i].index);
        picker_add(t, generation, c->name, c->keys, index);
        g_free(index);
    }
    if (n == 0)
        picker_message(t, generation, "No matching commands");
}

static void choose(App *t, const char *index)
{
    commands[atoi(index)].run(t);
}

void command_palette(App *t)
{
    picker_open(t, "Type a command", query, choose);
}
