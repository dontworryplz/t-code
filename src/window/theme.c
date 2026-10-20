/*
 * t-code's look: VS Code Dark Modern colors on rounded panels ("cards") that sit on the darker
 * frame of the title bar and the status bar. GTK's built-in theme is the base, not the desktop's
 * (Yaru, Breeze, ...), so t-code looks the same everywhere. This CSS goes over it, and the panels'
 * own CSS (window/statusbar.c, git/scm.c, popups/picker.c, ...) over this.
 */
#include "window/theme.h"

#define CARD_RADIUS "10px"

static const char *const css =
    /* The frame: title bar, status bar and the gaps between the cards. */
    "window.background { background: #181818; color: #cccccc; }"
    ".tcode-workspace { margin: 0 6px; }"
    "paned > separator { min-width: 6px; min-height: 6px; background: none; box-shadow: none; border: none; }"
    ".tcode-card { background: #1f1f1f; border-radius: " CARD_RADIUS "; }"
    ".tcode-card paned > separator { min-width: 1px; min-height: 1px; background: #2b2b2b; }"
    ".tcode-shell { padding: 6px 8px; }"

    /* Title bar: flat rounded buttons, and window controls of our own instead of the theme's
     * circles (the close button turns red, like on Windows and in VS Code). */
    "headerbar { min-height: 30px; padding: 0 4px 0 6px; background: #181818; box-shadow: none; border: none; }"
    /* As thin as VS Code's: the theme's own heights and paddings (46px in all) go. */
    "headerbar * { min-height: 0; margin-top: 0; margin-bottom: 0; padding-top: 0; padding-bottom: 0; }"
    "headerbar .title { font-size: 0.9em; font-weight: normal; color: #cccccc; }"
    "headerbar button { min-height: 22px; min-width: 24px; padding: 0 5px; margin: 4px 1px;"
    "  border-radius: 7px; background: none; box-shadow: none; border: none; color: #cccccc; }"
    "headerbar button:hover { background: rgba(255, 255, 255, .08); }"
    "headerbar button:active, headerbar button:checked { background: rgba(255, 255, 255, .14); color: #ffffff; }"
    "windowcontrols { border-spacing: 2px; margin-left: 8px; }"
    "windowcontrols > button { min-width: 36px; min-height: 22px; padding: 0; margin: 4px 0; border-radius: 7px; }"
    "windowcontrols > button > image, windowcontrols > button:hover > image,"
    "windowcontrols > button:active > image { background: none; box-shadow: none; padding: 0; margin: 0;"
    "  border-radius: 0; min-width: 16px; min-height: 16px; -gtk-icon-size: 14px; }"
    "windowcontrols > button.close:hover { background: #c42b1c; color: #ffffff; }"
    "windowcontrols > button.close:active { background: #a52617; color: #ffffff; }"

    /* Editor tabs: rounded on top, the current one in the editor's color, flowing into it. */
    "notebook { background: none; }"
    "notebook > header { background: #181818; border: none; box-shadow: none; padding: 4px 4px 0; }"
    /* Adwaita pulls the tabs over its header border (which t-code has not) with a negative margin: an
     * empty group then measured a negative height (a GTK warning). */
    "notebook > header > tabs { margin: 0; }"
    "notebook > header > tabs > tab { min-height: 30px; padding: 0 4px 0 12px; margin: 0 1px;"
    "  border-radius: 9px 9px 0 0; background: none; box-shadow: none; border: none; outline: none; color: #9d9d9d; }"
    "notebook > header > tabs > tab:hover { background: #222222; color: #cccccc; }"
    "notebook > header > tabs > tab:checked { background: #1f1f1f; color: #ffffff;"
    "  box-shadow: inset 0 2px #0078d4; }"
    "notebook > header > tabs > tab button { min-width: 20px; min-height: 20px; padding: 0; margin: 0;"
    "  border-radius: 6px; background: none; box-shadow: none; opacity: 0; }"
    "notebook > header > tabs > tab:hover button, notebook > header > tabs > tab:checked button { opacity: 1; }"
    "notebook > header > tabs > tab button:hover { background: rgba(255, 255, 255, .12); }"
    "notebook > header > tabs > tab label.temporary { font-style: italic; }"
    "notebook > header > arrow { border-radius: 6px; }"
    "notebook > stack { background: #1f1f1f; }"

    /* Everything else rounded too. */
    "button, entry, spinbutton, textview, .tcode-scm .message { border-radius: 8px; }"
    "tooltip, tooltip.background { border-radius: 8px; background: #252526; border: 1px solid #454545; color: #cccccc; }"
    "popover > contents, popover > arrow { background: #252526; border-color: #454545; }"
    "popover > contents { border-radius: 10px; }"
    "popover modelbutton { border-radius: 6px; }";

/* Before any window: GTK picks up the theme name when it styles the first one. */
void load_theme(void)
{
  GtkCssProvider *provider = gtk_css_provider_new();

  g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", "gtk-application-prefer-dark-theme", TRUE,
               NULL);
  gtk_css_provider_load_from_string(provider, css);
  /* Under the panels' own CSS, which uses GTK_STYLE_PROVIDER_PRIORITY_APPLICATION. */
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 1);
  g_object_unref(provider);
}

/* A rounded panel: its children (terminals, the minimap, the scrollbar) are cut to its corners. */
void make_card(GtkWidget *widget)
{
  gtk_widget_add_css_class(widget, "tcode-card");
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}
