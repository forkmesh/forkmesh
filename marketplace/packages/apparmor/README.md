# AppArmor

A desktop page that shows what AppArmor is doing on a Linux computer and lets
you manage it without a terminal.

- **Overview:** whether AppArmor is on, profiles by mode (enforce, complain,
  unconfined, disabled), running processes by confinement, a 24-hour chart of
  denials and accesses complain mode let through, and a list of things worth a
  look: noisy profiles, programs running unconfined although a profile matches
  them, profiles still in complain mode and profiles that are not loaded.
- **Profiles:** every profile in `/etc/apparmor.d` (and Snap's, read-only)
  with its mode, state, processes and denials. Switch it to enforce or
  complain, disable or enable it, reload or unload it, view it, or edit it.
- **Processes:** every process with its profile and mode.
- **Events:** the AppArmor log. Select denials and press **Allow…** to add the
  matching rules to the profile (its `local/` additions file when it has one)
  and reload it.
- **New profile…** writes a profile for a program in complain mode, so you can
  learn what it needs before enforcing it.

Reading the loaded profiles and the log needs administrator rights, so the
**Live view** button asks for your password once. Every change shows exactly
what will happen, asks for confirmation and then the administrator password,
keeps the replaced file in `/var/backups/forkmesh-apparmor`, and puts the old
file back if AppArmor rejects the new one.

Install it from the Market to add **AppArmor** to the desktop rail. It is
bundled but off by default. Details:
[docs/marketplace/apparmor.md](../../../docs/marketplace/apparmor.md).
