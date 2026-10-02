# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Cardinal rule: anything created outside `C:\GitHub` goes in `D:\claude_playground`

`C:\GitHub` is for repository working trees — clones and git worktrees — and nothing else.
**Everything else Claude Code creates on this machine goes under
`D:\claude_playground\<repo-name>\<task-slug>\`.** If it is not a checkout of a repo it belongs in
the playground, so there is one place to look and one place to clean. Never on the `C:\` root, never
inside a repo working tree, never scattered across user temp folders.

This covers test rigs and fleet runs, service working dirs (`XML/`, `bin/`, `logs/`, `*.pid`),
packet captures, exported CSV/JSON dumps, copied jars, DB export staging, scratch scripts, and
build or log output kept for later inspection.

- Make the task folder first: `D:\claude_playground\<repo-name>\<task-slug>\`, then write into it.
- Drop a one-line `NOTES.md` in each task folder saying what it is and when it was made.
- Repo working trees stay clean: nothing scratch gets written into them, nothing scratch gets committed.
- Never create a new top-level directory on `C:\`.
- The per-session scratchpad is for files that genuinely do not outlive the step that made them. Anything
  kept to look at again — a capture, a log, a build output, an export, a copied jar, a throwaway database
  cluster — goes to the playground instead, even though the scratchpad would accept it.
