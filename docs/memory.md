# Memory: what t-code costs, and a 250–300 MB idle report (Milis Linux)

A report came in on Milis Linux: t-code idle at 250–300 MB. On Fedora 44's `tools/headless.sh`
run of the same 25 files, the window plus helpers come to a third of that at the same point — so
the difference is worth finding before writing one more line of middle-size memory work. This is
the plan to find it.

## 1. Split the number first

`tools/proc-report.sh 10` (a run of the running process tree, PSS — shared pages split between the
processes):

* **t-code's own window** — expected ~57–83 MB by Fedora's run
* **clangd** — the usual top taker when files are open; 345–367 MB by Fedora's run *with the whole
  repository's 25 files open*, and nothing at all with none (`lsp/client.c`'s minute-long idle stop)
* every helper of 0.1.1-era (a micro per tab) is gone — 12 processes at 50 MB on Fedora, none now

Most "t-code eats 300 MB" reports are the third or the fourth row. If the window is the big one
and clangd is at ~0, the rest of this plan is about the window; if clangd is the big one, it is
about clangd's flags on Milis (step 3).

## 2. What changed since the report's session, cheaply (no code)

* `t-code --version` — which one the report was, so 0.3's known fixes (the `malloc_trim` on tab
  close, the `--pch-storage=disk` PCH, the two malloc arenas) are in on
* PSS or RSS: RSS counts shared pages it does not own (report tools differ wildly); the plan only
  reads numbers with PSS (`tools/proc-report.sh`), everything else counts twice or thrice
* the files open while idle: a session that restores 25 open tabs is not idle-even, and shows
  clangd its text of those shown
* GTK's font and icon caches: the Cascadia Code font (and Files panel's Nerd Font) rule here; a
  system that put the font in a slow, un-scoped, or duplicated font cache shows a bigger PSS
  for the window — compare with a plain `--` font once

## 3. Milis specifics worth asking for

* **musl or glibc?** Milis runs musl: where t-code's known wins are glibc ones, they make no
  difference there — `GLIBC_TUNABLES=glibc.malloc.arena_max=2` (clangd's two arenas) and
  `malloc_trim(0)` on tab close are no-ops, and musl's allocator is the malloc that only things
  that never happen must not grow. Recommend the number above for musl to know how much is ours
* **clangd's own flags:** on musl, `--pch-storage=disk` and the two indexing threads still apply;
  confirm the process table by proc-report shows them (a distro that packages clangd 14 instead of
  the newest versions changes the 300 MB figures as well)
* **VTE and the shells:** start t-code with the shell pane held up (Ctrl+J) and with it off, and
  report the two numbers — VTE's renderer holds a scrollback and Celt cache each; the difference
  is a direct measure of "the pane's share"

## 4. One more cut, if the window still comes back big

`tools/clangd-mem.py --churn 3` (how much of it is owed or freed), then the mix:

* `make test-asan` (the list in the Makefile) on the same session: an on-growth leak of the
  window's own heap is something the tests fail on — a leak is a bug, and the project's rule
  stands
* `valgrind --tool=massif` for one minute of idle (or `heaptrack`) names the top allocation by
  one — the only further step that reaches the answer if the window itself is at issue
* if nothing names one, then the number is spread (font caches, VTE, GTK's own theme): take the
  row from step 1 by process and start the piece below the biggest taker

## What a report should carry

As much as possible with one run — `tools/proc-report.sh 10 > report.txt`, one line each:
`--version`-found version, `ldd /usr/bin/t-code | grep musl`'s answer, the last five steps of an
opened-session count, the window's font settings applied then, and the answer of `echo
$GDK_BACKEND`. The number of rows t-code owns are on them; fixes start at the point of the
taker.
