# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Cardinal rule: rough work lives in `D:\claude_playground`

Every piece of scratch, throwaway, or intermediate work Claude Code does on this machine goes
under `D:\claude_playground\<repo-name>\<task-slug>\`. Never on the `C:\` root, never inside a
repo working tree, never scattered across user temp folders.

This covers test rigs and fleet runs, service working dirs (`XML/`, `bin/`, `logs/`, `*.pid`),
packet captures, exported CSV/JSON dumps, copied jars, DB export staging, scratch scripts, and
build or log output kept for later inspection.

- Make the task folder first: `D:\claude_playground\<repo-name>\<task-slug>\`, then write into it.
- Drop a one-line `NOTES.md` in each task folder saying what it is and when it was made.
- Repo working trees stay clean: nothing scratch gets written into them, nothing scratch gets committed.
- Never create a new top-level directory on `C:\`.
- The per-session scratchpad directory Claude Code is given is still fine for genuinely transient files.
