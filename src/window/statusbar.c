/*
 * The status bar at the bottom, like VS Code's:
 *   left   git branch ("*" when there are changes), clangd errors and warnings in the open files,
 *          Build and Run buttons and what they would build
 *   right  cursor line and column, and the Keybinds button
 */
#include "window/statusbar.h"

#include "dialogs/keybinds.h"
#include "editor/tabs.h"
#include "git/gitstatus.h"
#include "project/run.h"

static const char *const css =
    /* The title bar is in window/theme.c. */
    ".tcode-status { background: #181818; padding: 3px 6px; }"
    ".tcode-status button { min-height: 0; padding: 1px 8px; border-radius: 6px; background: none;"
    "  box-shadow: none; border: none; color: #cccccc; }"
    ".tcode-status button:hover { background: rgba(255, 255, 255, .12); }"
    ".tcode-status label { color: #cccccc; padding: 0 8px; }"
    ".tcode-status label.dim { color: #9d9d9d; }";

void update_status(App *t)
{
    int n, errors = 0, warnings = 0;
    Tab *tab;
    char *text;

    if (!t->status.build || t->closing)
        return;

    text = project_summary(t);
    gtk_label_set_text(t->status.build, text);
    g_free(text);

    n = tab_count(t);
    for (int i = 0; i < n; i++) {
        Tab *each = tab_nth(t, i);
        if (!each->compare_with) { /* old versions in a comparison do not count */
            errors += each->errors;
            warnings += each->warnings;
        }
    }
    text = g_strdup_printf("⊗ %d  ⚠ %d", errors, warnings);
    gtk_label_set_text(t->status.problems, text);
    g_free(text);

    tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->line > 0) {
        text = g_strdup_printf("Ln %d, Col %d", tab->line, tab->column);
        gtk_label_set_text(t->status.position, text);
        g_free(text);
        gtk_widget_set_visible(GTK_WIDGET(t->status.position), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(t->status.position), FALSE);
    }
}

void start_status_updates(App *t)
{
    git_status_start(t);
    update_status(t);
}

void status_show_branch(App *t, const char *branch, gboolean dirty)
{
    if (!t->status.branch)
        return;
    if (branch) {
        char *text = g_strdup_printf("⎇ %s%s", branch, dirty ? "*" : "");
        gtk_label_set_text(t->status.branch, text);
        g_free(text);
    }
    gtk_widget_set_visible(GTK_WIDGET(t->status.branch), branch != NULL);
}

/* Widgets ------------------------------------------------------------------------------------ */

static GtkLabel *status_label(GtkWidget *bar, gboolean dim)
{
    GtkWidget *label = gtk_label_new(NULL);

    if (dim)
        gtk_widget_add_css_class(label, "dim");
    gtk_box_append(GTK_BOX(bar), label);
    return GTK_LABEL(label);
}

/* A flat button on the bar that calls clicked(t). */
static void status_button(GtkWidget *bar, const char *text, const char *tooltip, GCallback clicked, App *t)
{
    GtkWidget *button = gtk_button_new_with_label(text);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_set_focusable(button, FALSE);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_signal_connect_swapped(button, "clicked", clicked, t);
    gtk_box_append(GTK_BOX(bar), button);
}

/* The Build and Run buttons. */
static void on_build(App *t)
{
    build_project(t, FALSE);
}

static void on_run(App *t)
{
    build_project(t, TRUE);
}

GtkWidget *status_bar(App *t)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkCssProvider *provider = gtk_css_provider_new();

    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    gtk_widget_add_css_class(bar, "tcode-status");

    t->status.branch = status_label(bar, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(t->status.branch), FALSE);
    t->status.problems = status_label(bar, FALSE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.problems), "clangd errors and warnings in the open files");
    status_button(bar, "⚙ Build", "Build (Ctrl+Shift+B)", G_CALLBACK(on_build), t);
    status_button(bar, "▶ Run", "Build and run (F5)", G_CALLBACK(on_run), t);
    t->status.build = status_label(bar, TRUE);

    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);
    t->status.position = status_label(bar, FALSE);
    status_button(bar, "⌨ Keybinds", "All keyboard shortcuts", G_CALLBACK(show_keybinds), t);
    return bar;
}
