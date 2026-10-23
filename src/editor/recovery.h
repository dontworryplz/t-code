/*
 * Crash Recovery: journals unsaved editor buffers and recovers them after a crash.
 */
#pragma once

#include "app.h"

/* Schedules a debounced snapshot write for a modified editor tab. */
void recovery_schedule_snapshot(Tab *tab);

/* Immediately writes a recovery journal snapshot for tab. */
void recovery_save_snapshot(Tab *tab);

/* Deletes the recovery journal for path (called on successful save or discard). */
void recovery_discard_snapshot(const char *path);

/* Checks if an unsaved snapshot exists from a previous session and prompts the user. */
void recovery_check_and_prompt(Tab *tab);
