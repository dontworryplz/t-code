/*
 * The Keybinds window (status bar button, or "Keyboard Shortcuts" in the command palette):
 * every shortcut in one place. The window's own come from the command table in window/commands.c.
 */
#include "dialogs/keybinds.h"

#include "window/commands.h"

static const KeyHelp tab_keys[] = {
    { "Switch to a tab", "Click" },
    { "Close a tab", "Middle click, ×" },
    { "Move a tab to the other group", "Drag it" },
    { "Open a compiler message's file and line", "Ctrl+Click in the shell" },
};

static const KeyHelp fm_keys[] = {
    { "Open folder / open file in a tab", "Click, Enter, l, →" },
    { "Collapse folder / go to parent", "h, Backspace, ←" },
    { "Move / scroll", "↑ ↓, j k, Wheel" },
    { "More (open to the side, preview, git, …)", "Right click" },
    { "New file (end with / for a folder)", "a" },
    { "Rename / delete", "r / d, Delete" },
    { "Cut / copy / paste files", "Ctrl+X / Ctrl+C / Ctrl+V" },
    { "Move into a folder", "Drag a file onto it" },
    { "Hide / show dotfiles / refresh", ". / R" },
};

static const KeyHelp editor_keys[] = {
    { "Save / close this tab", "Ctrl+S / Ctrl+Q" },
    { "Undo / redo", "Ctrl+Z / Ctrl+Y" },
    { "Copy / cut / paste", "Ctrl+C / Ctrl+X / Ctrl+V" },
    { "Select all / duplicate line", "Ctrl+A / Ctrl+D" },
    { "Find / next / previous", "Ctrl+F / F3, Ctrl+N / Shift+F3" },
    { "Move line up / down", "Alt+↑ / Alt+↓" },
    { "Comment line", "Alt+/" },
    { "Extra cursor at the next match", "Alt+N" },
    { "Completion", "Tab, Ctrl+Space" },
    { "Signature / definition / references", "Alt+K / Alt+D / Alt+R" },
    { "Format with clangd", "Alt+F" },
    { "micro's command bar (\"help\" for its help)", "Ctrl+E" },
};

static const char *const css =
    ".tcode-keybinds { background: #1f1f1f; }"
    ".tcode-keybinds .section { font-weight: bold; color: #e7e7e7; margin: 18px 0 6px; }"
    ".tcode-keybinds .action { color: #cccccc; }"
    ".tcode-keybinds .keys { font-family: 'Cascadia Code', monospace; font-size: smaller; color: #cccccc;"
    "  background: #313131; border: 1px solid #454545; border-radius: 4px; padding: 1px 6px; }";

/* A heading, then one row per shortcut: what it does on the left, the keys on the right. */
static void add_section(GtkGrid *grid, int *row, const char *title, const KeyHelp *keys, int n)
{
    GtkWidget *heading = gtk_label_new(title);

    gtk_widget_add_css_class(heading, "section");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_grid_attach(grid, heading, 0, (*row)++, 2, 1);
    for (int i = 0; i < n; i++) {
        GtkWidget *action = gtk_label_new(keys[i].name);
        GtkWidget *shortcut = gtk_label_new(keys[i].keys ? keys[i].keys : "—");

        gtk_widget_add_css_class(action, "action");
        gtk_label_set_xalign(GTK_LABEL(action), 0);
        gtk_widget_set_hexpand(action, TRUE);
        gtk_widget_add_css_class(shortcut, "keys");
        gtk_widget_set_halign(shortcut, GTK_ALIGN_END);
        gtk_grid_attach(grid, action, 0, *row, 1, 1);
        gtk_grid_attach(grid, shortcut, 1, (*row)++, 1, 1);
    }
}

/* Escape closes the window. */
static gboolean on_key(GtkEventControllerKey *controller, guint key, guint code G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    if (key != GDK_KEY_Escape)
        return FALSE;
    gtk_window_close(GTK_WINDOW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))));
    return TRUE;
}

void show_keybinds(App *t)
{
    static GtkWidget *window;
    GtkCssProvider *provider;
    GtkWidget *scroll, *grid;
    GtkEventController *keys;
    int n, row = 0;
    const KeyHelp *window_list = window_keys(&n);

    if (window) {
        gtk_window_present(GTK_WINDOW(window));
        return;
    }
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Keyboard Shortcuts");
    gtk_window_set_transient_for(GTK_WINDOW(window), t->window);
    gtk_window_set_default_size(GTK_WINDOW(window), 620, 720);
    gtk_widget_add_css_class(window, "tcode-keybinds");
    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_widget_set_margin_start(grid, 24);
    gtk_widget_set_margin_end(grid, 24);
    gtk_widget_set_margin_bottom(grid, 24);
    add_section(GTK_GRID(grid), &row, "Window", window_list, n);
    add_section(GTK_GRID(grid), &row, "Tabs and mouse", tab_keys, G_N_ELEMENTS(tab_keys));
    add_section(GTK_GRID(grid), &row, "File manager", fm_keys, G_N_ELEMENTS(fm_keys));
    add_section(GTK_GRID(grid), &row, "Editor (micro)", editor_keys, G_N_ELEMENTS(editor_keys));

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
    gtk_window_set_child(GTK_WINDOW(window), scroll);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(window, keys);
    gtk_window_present(GTK_WINDOW(window));
}
