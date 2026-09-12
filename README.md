# MoveTo

**Right-click files or folders → Move to → pick a recent or pinned folder.**

A Windows Explorer shell extension that adds a **Move to** submenu listing the
folders you actually move things to, so filing a download or a screenshot is
one hover and one click instead of a drag across two windows.

Built for 64-bit Windows 11. Installs **per-user without administrator rights**.

> **Status:** builds clean on VS 2022, 26 automated tests pass including a real
> end-to-end move. It is **not code signed** — see [Installing](#installing).

---

## What it does

- Lists your **recent** destinations, most recent first, and any folders you
  **pin** to the top.
- **Browse for folder…** is always there, so it works on the first run and
  seeds the history.
- **Manage destinations…** lets you pin, rename, reorder and forget entries.
- Works on a **mixed selection** of files and folders together.
- Moves are undoable with **Ctrl+Z** in Explorer.
- The first nine entries get **1**–**9** accelerators.

## Installing

Build it, then register **for your user only** — no elevation needed:

```bash
regsvr32 /n /i:user "C:\path\to\MoveTo.dll"
```

Or run `scripts\install-user.cmd`. For all users, run `scripts\install.cmd`
as Administrator.

Restart Explorer so it picks up the change:

```bash
taskkill /f /im explorer.exe && start explorer.exe
```

`scripts\uninstall-user.cmd` / `scripts\uninstall.cmd` reverse it.

> **The DLL is unsigned**, and Explorer loads it into its own process on every
> right-click. Build it yourself from source you have read, or don't install
> it. Sign it before putting it on machines that aren't yours.

> **Where's the menu item?** On Windows 11 it's under **Show more options**
> (or **Shift+F10**). Classic `IContextMenu` handlers don't appear in the short
> Windows 11 menu — that needs `IExplorerCommand` in a signed MSIX package.

## Building

Requires Visual Studio 2022 with **Desktop development with C++** and the
**C++ MFC and ATL** component.

```bash
msbuild MoveTo.sln /p:Configuration=Release /p:Platform=x64
```

Run the tests (Debug also enables the self-tests):

```bash
msbuild MoveTo.sln /p:Configuration=Debug /p:Platform=x64 && bin\x64\Debug\MoveToTests.exe
```

They build a tree in `%TEMP%`, exercise the preflight and one real move, and
clean up after themselves. Non-zero exit if anything fails.

## How it avoids moving the wrong thing

There is no preview dialog here — the whole point is one click — so the
safety has to live in the preflight. Every check below is covered by a test.

| Guard | What it stops |
|---|---|
| **`IFileOperation`, not `SHFileOperation`** | With one source and a destination that isn't an existing directory, `SHFileOperation` **renames the source to the destination's name and returns 0**. `report.docx` becomes an extension-less file called `Archive`. `IFileOperation` binds the destination first, so a missing one fails with nothing touched. |
| **Folder into its own descendant** | Refused. Identity comes from a real handle (`GetFinalPathNameByHandle`), not string comparison — `..`, 8.3 names, junctions, SUBST and mapped drives all defeat a lexical test, and the shell resolves them before acting. |
| **Nested selection pruning** | Selecting a folder *and* something inside it moves the parent first, leaving the child's path stale and the batch half-run. The child is dropped; moving the parent moves it anyway. |
| **Already in the destination** | Skipped, rather than prompting to overwrite a file with itself. |
| **Drive and share roots** | Refused as sources. |
| **Selection cap (10 000)** | Above it the operation is **refused outright**. Moving the first 10 000 of a larger selection and reporting nothing looks exactly like success. |
| **No `FOF_NOCONFIRMATION`** | The shell still asks before overwriting anything at the destination. |
| **Menu id arithmetic** | `QueryContextMenu` returns a claim on *id slots*, not a count of visible items. Get it wrong and Explorer hands your ids to the next handler — clicking "Move to > D:\Archive" runs someone else's menu entry. One pure function owns it, asserted on the first right-click. |

Anything that can't be moved is listed with the reason **before** the rest of
the batch runs, and you can back out.

## The destination list

Stored in `HKCU\Software\Wiziiot\MoveTo\Destinations`, one registry value per
folder — no `Recent0`/`Recent1` indices, so there is no index to alias when two
Explorer windows write at once. Ordering lives *inside* each value as a
timestamp, so promoting an entry rewrites exactly one value.

The menu path **never writes**: the list is loaded read-only in the
constructor, with no file-system access at all. Staleness is handled after a
move, never while a menu is being drawn.

A destination that's merely **unreachable** (VPN down, NAS asleep) is never
dropped — only one that genuinely isn't there, and only after two strikes.
**Pinned folders are never dropped automatically**, because a pin is user
intent and a disconnected morning shouldn't delete it.

## Privacy

The list accumulates folder paths you've moved things to, and it appears on a
menu in front of whoever is looking at your screen. **Manage destinations…** is
always present — not hidden behind a modifier key — so there is always a way to
remove an entry. Nothing leaves your machine.

## Known limitations

- **Windows 11 menu placement** — under *Show more options*.
- **No copy variant.** Move only, by design.
- **Paths ≥ `MAX_PATH`** as destinations are refused rather than silently
  mishandled.
- **The preflight runs on Explorer's UI thread** behind a wait cursor. It's
  bounded by the selection cap, but a huge selection on a slow network source
  will pause the menu.
- **Unsigned.** See [Installing](#installing).

## Credits and licence

The shell-extension plumbing — `IShellExtInit`/`IContextMenu` structure,
registration, the `DragQueryFile` and data-object handling — comes from
[DirClean](https://github.com/ram-wiziiot/DirClean), which is itself a port of
**Michael Dunn's** DirClean (CodeProject, 2000–2002). His notice travels with
that code and is retained in the files derived from it.

New work in this repository is MIT. See [LICENSE](LICENSE).
