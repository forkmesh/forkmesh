# Shortcuts: one-tap runs and the queue

Shortcuts are the scripts and prompts in a repository's `.forkmesh/shortcuts/`
folder. The desktop runs scripts (`bash`, from the repository root); the
iPhone and Android apps list them and run them on the desktop over the phone
LAN control link (`insights.*` family, see [networking.md](networking.md)).
The feature is the built-in `shortcuts` package
([../marketplace/core-plugins.md](../marketplace/core-plugins.md)).

Code: `desktop/src/MainWindowShortcuts.cpp` (page, runs, queue),
`desktop/src/MainWindowLocalControlInsights.cpp` and
`desktop/src/LocalControlInsights.cpp` (phone API),
`native/ios/ForkMeshCore/Sources/FMTeammates/ShortcutsStore.swift` and
`native/ios/ForkMesh/Features/Teammates/ShortcutsScreen.swift`,
`native/android/core/.../teammates/ShortcutsStore.kt` and
`native/android/app/.../feature/teammates/ShortcutsScreen.kt`.

## Starter shortcuts

Shortcuts live in each repository, so someone else's repository shows none
until it has its own `.forkmesh/shortcuts/` folder. Users asked on 2026-10-29
why the shortcuts did not show for another user, so the Shortcuts package now
ships five general-purpose prompt shortcuts that work in any repository:

| File | Name |
| --- | --- |
| `review-changes.md` | Review my changes |
| `audit-user-experience.md` | Audit and improve user experience |
| `clean-up-stale-code.md` | Retire stale code and performance debt |
| `refresh-documentation.md` | Refresh the documentation |
| `propose-feature-removal.md` | Propose a feature to retire |

They are cleaned-up versions of this repository's own prompts: no ForkMesh
paths, teammates or pinned `# agent:`/`# model:` lines, so they run with the
user's default agent. The source is
`marketplace/packages/shortcuts/content/shortcuts/`, and two surfaces offer
it:

- **Desktop.** The files are embedded as `:/starter-shortcuts/<file>`
  (`desktop/resources/icons.qrc`). When the selected repository has a working
  tree and its `.forkmesh/shortcuts/` folder is empty or missing, the page
  header shows **Add starter shortcuts**. It copies every starter file that is
  not already there and leaves existing files alone, then says to commit them
  so everyone who uses the repository gets them. It is hidden on read-only
  mirrors and in repositories that already have shortcuts, so a repository
  with its own versions (like this one) does not get near-duplicates. Code:
  `missingStarterShortcuts` and `addStarterShortcuts` in
  `desktop/src/MainWindowShortcuts.cpp`.
- **Web.** The package declares each file as a `content.shortcut`
  contribution, so the Workflows Repository view lists them for accounts with
  the Shortcuts package installed and adds one as a commit after review
  ([../marketplace/README.md](../marketplace/README.md#protocols-and-interfaces)).
  Because the package now has a `content/` folder, it is a selectable plugin
  and is listed in `marketplace/plugins.json`.

The phones list whatever the desktop's folder holds, so the starters show on
both phones once added. `test_shortcuts_plugin_ships_starter_shortcuts_the_desktop_embeds`
in `app/tests/test_workflows.py` checks that every file parses without
warnings, has a `# name:` matching the manifest, mentions no ForkMesh
specifics, and is embedded by the desktop.

`.forkmesh/shortcuts/code-review.md` had no header and asked the user to
paste code, which a one-tap run cannot do; it now holds the same text as
`review-changes.md`.

## One tap from the list

Every platform runs a shortcut straight from its list:

- **Desktop, cards**: clicking a card runs the script, drafts the prompt, or
  starts its dedicated agent (`# agent:`).
- **Desktop, table**: each runnable row has a play button in its first column;
  double-click still works.
- **iPhone**: each runnable row has a play button. It no longer opens the
  detail page or the confirmation sheet first. Prompt shortcuts that run on
  the phone open the composer the same way the detail page does.
- **Android**: the row's **Run** button runs it without the confirmation
  dialog.

The phones still use the desktop's two-step `insights.prepare` /
`insights.run` token exchange, since it ties each action to the paired device.
They just send both steps in one tap and skip the sheet. The user asked for
one-tap runs, and the list already shows what each row does. The detail page
on iPhone keeps its **Run** button with the sheet.

A run started from a paired phone counts as the user's approval of that
file. The desktop records the approval for the file as it is now, the same
store its **Allow repository workflows?** window writes, logs "approved on
this desktop by a run from <phone>" and runs it. The same goes for running a
workflow from the phone (`workflows.run`). The user asked for this on
2026-10-29: the phone is their own device and should not depend on someone
clicking Allow at the desktop. Unattended runs (schedules, repository events,
special-agent schedules) still wait for an approval, and a later change to
the file asks again (S-44 and S-70 in
[../security/findings.md](../security/findings.md)). An older desktop still
answers `approval_required`; the phones then say to update ForkMesh on the
desktop.

A workflow's **Run shortcut** step runs a script or prompt shortcut too,
waits for it and hands its output to the next step, so a shortcut can run on a
schedule, after a special agent or from a button pinned to any rail
([../app/workflows.md](../app/workflows.md#desktop-workflows-and-special-agents)).

## The queue

Script shortcuts can be queued behind the ones already running, so a
"build, then deploy" pair runs in order without waiting by the screen.

- The queue is kept per repository (`owner/name`). A queued script starts once
  no script shortcut is running in that repository. Queued scripts start one
  at a time, in the order they were queued. Runs in other repositories are
  not affected.
- Queueing when nothing is running in that repository starts the script at
  once (`queueShortcut` returns position 0).
- Queueing a script that is already queued does nothing and returns its
  current position.
- A queued script starts whether the run ahead of it succeeded, failed, or was
  stopped. Remove it from the queue to stop it from starting.
- Clicking a running card still restarts it (stop, then run again). A pending
  restart goes before the queue.
- Only scripts can be queued. Prompt and skill entries start agents, which
  have their own queue.
- The queue lives only in memory. Quitting the desktop clears it.

Desktop UI: while a script runs, the script cards show a queue button
(`list-unordered`). A queued card shows a `queued #n` pill and a button that
removes it from the queue. The table shows `queued #n` in **Took**, and its
right-click menu offers **Queue after the running shortcuts** or **Remove from
queue**. The output bar shows **Up next** and a **Clear queue** button.

Phones: `insights.shortcuts` entries carry `queued` (their position, 0 if not
queued). While a script runs in the repository, script rows show a queue
button, and queued rows show `Queued #n` with a remove button.

Phone API: the actions `queueShortcut` and `unqueueShortcut` take the same
`repo`/`scope`/`file` subject as `runShortcut`. They are confirmed actions
(`mutatingActions`) and only accept script shortcuts (`not_a_script`
otherwise).

## Steps and linked shortcuts

A multi-step script, such as `deploy-new-version.sh`, can name its steps and
point at the shortcuts it runs, so its card reads like a workflow (the deploy
shortcuts are described in [../release/README.md](../release/README.md#build-numbers)). The desktop,
the phones and the web canvas (`app/src/workflow_files.py`) follow the same
rules:

- The header is an optional `#!` first line followed by the `# name:`,
  `# description:`, `# agent:` and `# model:` lines (at most 20 lines). The
  rest of the file is split into blocks on blank lines.
- A block whose first line matches `^#\s*step:\s*(.+?)\s*$` is a named step.
  Its name is that capture with whitespace collapsed, cut to 80 characters.
  Unnamed blocks are not steps.
- A step links to another shortcut when one of its lines matches
  `^\s*bash\s+\.forkmesh/shortcuts/([A-Za-z0-9._-]{1,96})(?:\s|$)`. The first
  such line wins. The linked file is in the same repository's
  `.forkmesh/shortcuts/` folder.
- At most 32 steps are read. Only scripts have steps; skills and prompts never
  do.
- `# step:` lines are never taken as the description, even when there is no
  `# description:` line.

```sh
# step: Reset build numbers to 0
python3 -u tools/forkmesh-release-host.py --reset-builds --tag "$tag"

# step: Deploy Android build
bash .forkmesh/shortcuts/deploy-build-android.sh --first-build
```

Code: `parseShortcutSteps` in `desktop/src/LocalControlInsights.cpp` (tested
in `desktop/tests/test_local_control_teammates.cpp`), `ShortcutStepList` and
`revealShortcutFile` in `desktop/src/MainWindowShortcuts.cpp` (the card and its
link are checked by `forkmesh-window-tests --teammates-only`;
`FORKMESH_SHORTCUT_STEPS_SHOT=<file.png>` saves the card).

**Cards.** Under the description, a card draws its steps as a compact vertical
list in the style of the Actions page's step dots: a dot per step joined by a
line, then the step name. A linked step has a filled dot in the accent colour
and an accent `→ <title>` label after its name, showing the linked shortcut's
`# name:` (or its file name). Clicking the label scrolls to that shortcut's
card and outlines it in the accent colour for a moment. It does not run
anything. If a search or the skills-only filter hides the card, both are
cleared first. A link to a file that is not in the folder shows a notice.
Clicking anywhere else on the list runs the card as usual. A card shows at
most six rows: past six steps it shows the first five and `+N more`. The
tooltip lists every step.

**Table.** The **Type** column reads `script · N steps`. Its tooltip, and the
name's, lists the steps. The right-click menu has **Go to linked <file>** for
each linked shortcut, which selects that row.

**Progress.** The script runs as one `bash` process, so the desktop cannot
tell from the output alone which step is running (output varies, and nested
`bash .forkmesh/shortcuts/...` runs are not seen). A script reports its step
itself by printing a marker line as each step starts:

```sh
# step: Remove agent leftovers
echo "::step::Remove agent leftovers"
python3 tools/free_disk_space.py --execute --step agents
```

- A marker is a whole output line `::step::<name>` (surrounding whitespace,
  a trailing `\r` and ANSI colour codes ignored). `<name>` is matched against
  the parsed step names without regard to case, with whitespace collapsed; a
  number `1`..`N` also works. Anything else, including a marker with text
  before it, is ignored. The last marker seen wins, so a step can be marked
  again or out of order.
- While the run is going, steps before the current one get a green dot, the
  current one an accent dot with a halo and a bold name, later ones keep
  their usual dot. The output bar's status reads
  `running <file>… step K of N: <name>`.
- When the run ends, a success marks every step done; a failure marks the
  current step red; a stopped run greys it. The card keeps that picture for
  the newest of the last 20 runs (in memory only), so the step that failed
  stays visible after the run.
- If the current step is hidden behind `+N more`, that row reads
  `+N more · step K: <name>` in the step's colour.
- A script without markers shows no per-step progress; the overall progress
  bar is unchanged either way.
- Phone run rows (`insights.shortcutRun`, `insights.shortcutOutput`) carry
  `step` (0-based, -1 before the first marker) and `stepCount` when the
  script has steps.

Code: `shortcutStepMarker` and `trackShortcutStep` in
`desktop/src/LocalControlInsights.cpp` (tested in
`desktop/tests/test_local_control_teammates.cpp`), `ShortcutStepList::setProgress`
in `desktop/src/MainWindowShortcuts.cpp`.

**Phones.** Each `insights.shortcuts` entry with at least one step carries
`"steps": [{"name": "Reset build numbers to 0"}, {"name": "Deploy Android build",
"shortcut": "deploy-build-android.sh"}]`. `shortcut` is present only on a
linked step and names a file in the same repository and scope as the entry.
Entries without steps have no `steps` key.
