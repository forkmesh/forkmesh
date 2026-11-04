# AppArmor

A desktop page that shows what AppArmor is doing on a Linux computer and lets
you manage it without a terminal.

Everything is on one screen:

- **Charts:** the share of running processes AppArmor confines, profiles by
  mode, a 24-hour chart of denials and accesses complain mode let through, and
  the profiles that block the most.
- **Profiles** and the **selected profile:** every profile in
  `/etc/apparmor.d` (and Snap's, read-only). Pick one to see what the program
  is, what its mode means, what to do next (the suggested action is
  highlighted) and a 7-day chart of its events. Switch it to enforce or
  complain, disable or enable it, reload or unload it, view it, or edit it.
- **Processes:** every process with its profile and mode.
- **Events:** the AppArmor log. Select denials and press **Allow…** to add the
  matching rules to the profile (its `local/` additions file when it has one)
  and reload it.
- **Suggestions:** noisy profiles, complain profiles to review or enforce,
  programs running unconfined although a profile matches them, profiles that
  are not loaded, and the busiest programs without a profile.
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
