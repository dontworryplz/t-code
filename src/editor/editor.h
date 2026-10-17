/*
 * Editors: micro running in a tab's terminal. Opening a file makes a tab whose micro starts the
 * first time the tab is shown (a restored session with many tabs starts only the visible ones).
 * micro reports its cursor, problems and viewport through the terminal title and takes commands
 * through a named pipe (control.h); t-code saves changed editors every second (autosave).
 */
#pragma once

#include "app.h"

/* Opening ------------------------------------------------------------------------------------ */

/* Opens path in a new tab of the active group; micro starts once the tab is shown. line > 0 puts
 * the cursor there. extra_env ("NAME=value", may be NULL) goes to micro's environment. */
Tab *open_editor(App *t, const char *path, int line, int column, gboolean readonly, char **extra_env);

/* Opens path in an editor, or switches to the tab that has it already. */
void open_file(App *t, const char *path);

/* The same, and puts the cursor at line and column (1-based; line 0: where it is). */
void open_file_at(App *t, const char *path, int line, int column);

/* Opens the file in the second group, splitting first. An already open file moves there, so the
 * same file is never edited by two editors at once. */
void open_to_side(App *t, const char *path);

/* Talking to micro --------------------------------------------------------------------------- */

/* Runs a micro command in an editor ("save", "goto 12:1", "setlocal tabsize 4"): through the
 * plugin's pipe, or else typed into its command bar (Ctrl+E). */
void editor_command(Tab *tab, const char *command);

/* Types keys into an editor after a lone Escape that closes any open prompt. */
void feed_editor(Tab *tab, const char *keys);

/* Asks micro to quit, which asks about unsaved changes first; the tab goes when micro exits. */
void quit_editor(Tab *tab);

/* Saving ------------------------------------------------------------------------------------- */

typedef void (*SavedFunc)(App *t, gpointer data);

/* Saves every changed editor, then calls then(t, data) (also when a save does not finish within
 * ~450ms, and while the app closes: then checks t->closing itself). */
void save_all(App *t, SavedFunc then, gpointer data);

/* Starts saving changed editors once a second, when the autosave setting is on. */
void start_autosave(App *t);

/* Turns code coloring off or on in every open editor (F7); new ones start the same way. */
void toggle_syntax(App *t);
