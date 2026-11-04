# AppArmor package

`marketplace/packages/apparmor` is a bundled desktop package (Market id
`apparmor`, entrypoint `builtin:apparmor`) that shows what AppArmor is doing
on a Linux computer and manages it from a GUI. It is off by default and adds
**AppArmor** (octicon `shield-check`) to the rail when installed. On macOS and
Windows the page says AppArmor is Linux only. The [Security
package](computer-security.md) checks only whether AppArmor or SELinux is on;
this package is where you manage AppArmor itself.

## What the page shows

One screen, no tabs: a strip of charts on top and three columns of panes
below, so the state, the selected profile and its log are in view together.

| Pane | Content |
| --- | --- |
| Coverage | A ring with the share of running processes AppArmor confines, and stacked bars of profiles by mode and processes by confinement. |
| Last 24 hours | Denials (red) and accesses complain mode let through (amber) per hour, with a tooltip per hour and the 7-day totals. |
| Busiest profiles | The five profiles with the most events in 7 days as bars; a click selects the profile and filters Events to it. |
| Profiles | One row per top-level profile in `/etc/apparmor.d` and `/var/lib/snapd/apparmor/profiles`, plus loaded profiles that have no file: a coloured mode dot (with the state when it is not the usual one), running processes, denials. The tooltip names the program. |
| Selected profile | What the program is (`programDescription`, a short list of common profile names), what its mode means, **suggestions** for it, a 7-day chart of its own events, the actions, the attachment, file, flags and `local/` additions file, and **Show file** for the rules. |
| Processes | Every process with a command line: mode, label, and "Restart to confine" when it runs unconfined although a profile's attachment matches its executable. Double-click opens the profile. |
| Events | The AppArmor log for the last 7 days, newest first (at most 5,000 rows): denied, allowed (complain mode), and profile loads, optionally only the selected profile's. Selecting events shows the rule that would allow them beside **Allow…**. |
| Suggestions | The page-wide to-do list, each with a link: read the log, the profiles that block the most, complain profiles that let accesses through or are ready to enforce, processes that started before their profile, profiles only logging, profiles not loaded, and the busiest unconfined programs. |
| System | Kernel, security modules, `apparmor_parser`, policy files, loaded profiles and log source, each with a tick or a cross. |

The top search box filters all three tables and finds profiles.

### Suggestions for one profile

`profileAdvice` (in `AppArmorModel`) turns a profile's facts (mode, file,
disabled, loaded, unconfined flag, whether the log was read, processes,
processes that started before it, denials, accesses let through) into
plain-language advice, each with a tone and an optional action. The first
action is shown as the primary button: Enable for a disabled profile, Reload
for one that is not loaded, Show events when an enforce profile blocked
something or a complain profile let something through, Enforce for a running
complain profile with a quiet log. Profiles flagged `unconfined`, Snap's and
those without a file get an explanation instead of an action.

## What is readable without a password

On current kernels `/sys/kernel/security/apparmor/profiles` (the loaded
profile list) is root-only even though its mode is `0444`, and the kernel
journal and `/var/log/audit/audit.log` are readable only by root or the
`adm`/`systemd-journal` groups. Everything else is readable by the user:
`/sys/module/apparmor/parameters/enabled`, `/sys/kernel/security/lsm`, the
profile files, and `/proc/<pid>/attr/apparmor/current` (falling back to
`/proc/<pid>/attr/current` on older kernels).

So **Refresh** reads the files and processes (and the logs when the user can)
on a worker thread, and the page shows the mode a profile **declares**.
**Live view** runs one `pkexec` script that prints the loaded profile list and
the AppArmor lines of the audit log and kernel journal; after that the page
shows the **live** mode and Loaded / Not loaded. Live view changes nothing.
The page never polls.

Every administrator action also prints the loaded profile list at the end
(`@@profiles` section), so the live state stays current after a change without
a second password prompt. pkexec's default polkit action does not keep the
authorisation, so a second read would ask again.

## Actions

Every action shows a confirmation with the exact change (a line diff for file
edits) and then the system's administrator prompt (`pkexec /bin/sh -c
<script> sh <arguments>`). File names and paths are passed as positional
arguments, never pasted into the script text.

| Action | What runs |
| --- | --- |
| Enforce / Complain | Rewrites the `flags=(…)` of every top-level profile in the file (keeping other flags such as `attach_disconnected`; enforce drops the clause when nothing is left), installs it and reloads it. Like `aa-enforce`/`aa-complain`, but without needing `apparmor-utils`. |
| Disable / Enable | Disable links the file into `disable/` and unloads it. Enable moves the link to the backup folder (not deleted) and loads the profile. |
| Reload / Unload | `apparmor_parser -r -W <file>` / `apparmor_parser -R <file>`. Unload lasts until the next reload or restart. |
| Edit… | An editor for the file; Save and load installs and reloads it. |
| New profile… | Writes a profile for a program from a template (the newest `abi` installed, `tunables/global`, `abstractions/base`, the program `mr`, `include if exists <local/NAME>`) in **complain** mode, then loads it. The name must be letters, digits, `.`, `_`, `+` or `-` and must not exist yet. |
| Allow… (Events) | Turns the selected events of one profile into rules and adds them to its `local/` additions file when the profile includes one, otherwise just before the profile's closing brace; then reloads the profile. Rules already present are skipped. |
| Reload all | `systemctl reload apparmor`. |

**Writing a file is all-or-nothing.** `adminInstallScript` copies the current
file to `/var/backups/forkmesh-apparmor/<path with / as _>.<timestamp>`,
installs the new one, and reloads the main profile. If `apparmor_parser`
rejects it (exit 3), it copies the backup back, or moves a new file aside as
`.rejected.<timestamp>`, so a broken profile never stays in
`/etc/apparmor.d` to fail the next boot. Nothing is ever deleted; the backup
folder is outside `/etc/apparmor.d` because the AppArmor loader would try to
parse anything left there.

### Suggested rules

`suggestedRule` covers the common denials:

- files: the denied mask becomes permissions (`c`/`d` → `w`, `a` only without
  `w`, `x` → `ix`, plus `m`, `r`, `l`, `k`). Glob characters in the path are
  escaped and paths with spaces, commas or `#` are quoted;
- `capability <name>,`, `network <family> <type>,`,
  `signal (<mask>) set=(<signal>) peer=<peer>,`, `ptrace (<mask>) peer=<peer>,`
  and `dbus` method calls.

A value with anything other than letters, digits and `_.:/@+-` (or a path
with a newline) gives no rule, so a crafted file name or D-Bus member in a log
line cannot inject extra rules into a profile. Events of a child profile or
hat (`parent//child`) are not allowed automatically; they need Edit. `ix` is
the conservative exec choice: it keeps the child under the same profile.

### What the page refuses

- **Profiles flagged `unconfined`** (Ubuntu ships many, for example
  `firefox`, only to give an unprivileged-user-namespace exception a name):
  Enforce and Complain are disabled. Those profiles have no rules, so either
  mode would break the program.
- **Snap profiles** are view-only: snapd regenerates them.
- Without `apparmor_parser` or `pkexec`, the actions are disabled or report
  why.

## Parsing notes

`parseProfileDeclarations` scans the file once, skipping quoted strings,
escapes and `#` comments (a `#` at line start or after whitespace; `#include`
is still found for the `local/` lookup). A `{` opens a **block** only when it
follows whitespace and is followed by whitespace or the line end; any other
`{` is a glob alternation (`/{usr,opt}/x`, `firefox{,-esr}`, `@{HOME}`) and
its `}` closes the glob. Only depth-0 blocks whose header starts with
`profile`, `/`, `@{`, `"` or `:` are profiles, so hats, child profiles and
conditionals are left alone.

The "unconfined but a profile exists" check turns each attachment into a
regular expression (`**` crosses folders, `*` and `?` do not, `{a,b}`
alternation, `[…]` classes) once per refresh. An attachment that starts with
a variable never matches, to avoid guessing.

Log lines from `audit.log` and from `journalctl -k` are parsed the same way:
the time and the serial come from `audit(<seconds>.<ms>:<serial>)`, and an
event seen in both logs is counted once. Unquoted `name`, `comm`, `profile`
and similar fields are hex-decoded, the way the audit system writes values
containing spaces.

## Code and tests

| File | Responsibility |
| --- | --- |
| `AppArmorModel.{h,cpp}` | Pure parsing and editing: profile declarations, flags, rule insertion, templates, globs, log events, process labels, hourly buckets, program descriptions and per-profile advice. |
| `AppArmorSystem.{h,cpp}` | Reads a snapshot from a root folder (`/` in the app, a fixture tree in tests) and holds the administrator scripts. |
| `AppArmorFeature.{h,cpp}` | The page and its panes, the chart widgets (`CoverageRing`, `ModeBar`, `TimelineChart`, `RankBars`, coloured with the theme's success, warning and danger roles), dialogs and the pkexec runner. |

`forkmesh-apparmor-tests` (`tests/test_apparmor.cpp`) covers the parser on
real-world profile shapes, mode rewriting, rule insertion, templates, globs,
every suggested-rule kind and the injection refusals, program descriptions
and per-profile advice, a fixture snapshot
(leftover `.dpkg-old` files, `disable/` links, Snap profiles, kernel threads),
and runs the install script against a fake `apparmor_parser` to check the
backup and the rollback.
