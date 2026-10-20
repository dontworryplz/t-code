/*
 * git status for the project, run in the background: when the repository changes (.git/HEAD or
 * .git/index, watched), when an editor saves, and every 10 seconds for changes made outside
 * t-code (a build, a shell command). Each answer goes to the status bar's branch label and to
 * the Source Control panel (scm_update).
 */
#pragma once

#include "app.h"

/* Watches the project's repository and starts the 10 second timer. Once, at start. */
void git_status_start(App *t);

/* Watches the git directory of the project (t->project.root) and finds the top of its work tree
 * (t->project.git_top: NULL at once, set when git answers, and stays NULL outside a repository);
 * call again after the root changes. */
void watch_git(App *t);

/* Runs git status shortly; changes that come together (a commit touches several files) share
 * one. */
void refresh_branch(App *t);
