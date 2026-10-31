# Busy ring: a spinner around the icon while something runs

Since 2026-10-31 every platform shows the same "working" mark: a thin ring
around a button's icon whose bright arc turns once every 1.6 s and fades over
70 % of the circle, on a faint 14 % track. It is the activity halo the phone
apps' bottom bar already drew around an active item (`ShellActivityHalo` on
iOS and Android), now also on buttons and on the left rails. A button with no
icon gets the same sweep around its rounded outline instead.

The user asked for it in one sentence: "when we click on any button that does
something show a spinner around the icon please on all platforms; also when
shortcuts are running show the spinner on the left rail; show any active left
rails with a spinner on the icon similar to how we do in mobile".

## The rule: disabled right after a click means working

No platform can tell in general when a click's work ends, but almost every
button that starts slow work disables itself until the work is done (a Qt
`setEnabled(false)`, a SwiftUI `onPressed: busy ? nil : action`, a Compose
`enabled = !busy`, a web `button.disabled = true` or `aria-busy`). So the rule
is the same everywhere: **a button that is disabled right after it is clicked
shows the ring until it is enabled again**, capped at 60 s so a button that
stays disabled for good (the last page of a list) does not spin forever.
Checkboxes, radio buttons, toggles, checkable buttons and buttons that open a
menu are left out. Callers that know better can force it on or off.

One exception keeps Send buttons quiet. A Send button is usually disabled
while its message box is empty, so clearing the box after a send disables it
without any work running. On desktop and web, the text field that had focus
when the button was pressed is remembered: if it is empty after the click, the
button does not ring, and a ring that is already turning stops as soon as that
field empties (a send that finished and cleared the box). On the phones the
kit buttons take an explicit `busy` argument, and composer and form buttons
whose enablement also depends on their text pass their real busy flag.

## Desktop

Code: `desktop/src/BusyRing.{h,cpp}`.

- `busyring::paint` draws the ring (a `QConicalGradient` stroke) around a
  rectangle; `busyring::phase()` reads one shared clock, so every ring on
  screen turns in step.
- `busyring::setBusy(widget, on, anchor)` marks any widget busy. Ordinary
  buttons get a mouse-transparent child overlay that follows the button's
  size and draws the ring around the icon (`busyring::iconRect`, which follows
  how Qt lays out icon and text), around `anchor` when one is given (the
  Shortcuts cards ring their 16 px icon label), or around the outline when
  there is no icon. Widgets that paint themselves (`fmBusyPaintsSelf`, set by
  `ActivityRailButton`) only receive the `fmBusy` property and draw the ring
  in their own `paintEvent`.
- `busyring::installClickWatcher()` (called from the `MainWindow`
  constructor) is one application event filter that looks only at left-button
  releases and Space releases on buttons; two event-loop turns later, so work
  that a handler defers with `QTimer::singleShot(0)` counts, a button that is
  now disabled becomes busy until its `EnabledChange` back to enabled.
  `busyring::watchClick` does the same for a programmatic `click()`.
- `startButtonSpin` / `stopButtonSpin` (Git sync, pull all, unsynced commits)
  now show the ring around the button's own icon instead of swapping it for a
  spinning refresh glyph. The restart buttons keep their own progress glyph,
  which shows how far the rebuild has got.

### Left rail

`ActivityRailButton` shows the ring around its icon when any of these is true
(`ringShown()`), in Classic and in Next (`RailButtonPaintState::busy`, painted
by `paintRailButtonHook`), in the icon column and in the expanded rows:

- `setRailActive(true)`: the section has work running, the same signals the
  phone apps use for their `active` items. **Agents** is active while any
  agent session is running (`updateAgentsNavBadge`,
  `updateAgentsTabIndicator`); **Code** is active while any shortcut runs
  (`refreshShortcutRailActivity`, called when a run starts and finishes).
- `setActivityBlink(true)`: the deploy activity of **Control** and
  **Network**. This used to blink a small green lamp; the ring replaces it so
  there is one "working" mark.
- the button itself is busy (`fmBusy`), for example after the click watcher
  saw it disable itself.

A pinned page shortcut on the rail (`railShortcutButton`) mirrors the page
button it presses: `MainWindow::refreshRailShortcutBusy` runs whenever any
busy state changes (`busyring::observe`) and rings each tile whose
`pageButtonForShortcut` is busy. A shortcut's own Run button in the Shortcuts
table and its card ring while that shortcut's process runs.

## Web dashboard

Code: `app/public/dashboard/js/02-helpers.js` (`setButtonBusyRing`,
`watchButtonBusy`, `setNavWorking`) and the `.fm-busy-ring` rules in
`app/public/dashboard/shell.html`.

- A capturing document `click` listener applies the rule above to every
  `<button>`; a `MutationObserver` on `disabled`, `aria-busy` and the
  button's children removes the ring when the button is usable again (and
  puts it back if the button's markup is replaced while it is still busy).
  `data-no-busy` opts a button out.
- `setSubmitBusy` keeps the button's icon and rings it, with the progress
  label beside it; a button without an icon keeps the old `fm-spinner`.
- The ring is a `span` placed over the icon's box; its colour is `--primary`
  (blue, the theme's role for focus and activity), drawn as a conic gradient
  masked to a 1.75 px stroke and turned through the registered
  `--fm-busy-turn` angle, so the same CSS also sweeps a rounded outline.
  Reduced motion shows a still ring.
- `setNavWorking(nav, on)` rings a sidebar entry's icon; the Agents page turns
  it on for **Agents** while one of the listed sessions is running.
