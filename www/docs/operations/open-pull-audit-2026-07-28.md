# Open pull-request audit method

An audit of open pull requests evaluates each signed record whose status is
`open` against the current `main`:

- whether the exact head still exists on the authoritative remote;
- whether the stored patch applies to the current index, and whether the
  reverse patch applies (the change is already represented);
- for a live head, `git diff --check` and a `git merge-tree` conflict check;
- a conservative content/path scan (credential paths, dynamic code, HTML
  sinks, shell downloads) that marks the change for security review. A trigger
  is not itself a vulnerability finding.

Each record gets one disposition:

| Disposition | When |
|---|---|
| Merge | The head is live, merges cleanly, and focused tests pass. |
| Hold | The head is gone but the patch still applies: recreate a signed branch and review it before any merge. |
| Close | The patch conflicts, is empty, is already in `main`, or is superseded. |

The audit never edits signed pull metadata by hand: merged pulls close through
normal pull synchronization, and other closures go through the authenticated
pull-request workflow.
