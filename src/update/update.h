/*
 * update.h — the new-release check: updates.c offers it.
 */
#pragma once

#include "app.h"

/* Once, a few seconds after a window shows (and only when the setting asks for it): the latest
 * GitHub release's version goes against t-code's; a newer one opens a small window whose button
 * leads to the release page. Also "Check for Updates Now" in the command palette. */
void updates_start(App *t);

/* The same check, right now ("Check for Updates Now"): it asks even when the setting is off, and
 * the answer shows even when it found nothing new. */
/* Compares two version strings: TRUE when theirs is newer than ours. */
gboolean newer_version(const char *ours, const char *theirs);
void updates_check_now(App *t);
