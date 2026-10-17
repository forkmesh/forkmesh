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
