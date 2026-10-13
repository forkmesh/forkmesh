# Releases without the desktop dialog (2026-10-01)

Part of [the tools docs](README.md); the release pipeline itself is in
[../release/README.md](../release/README.md).

## Why

Cutting v1.2.7 had no route that did not end in the desktop's "Draft a new
release" dialog, where a person had to press Publish release:

- the phone's Create release button only showed on an existing release card,
  and the desktop answered it with an approval prompt and then that dialog,
  pre-filled;
- the MCP connector had no release tool at all.

The owner asked to "fully manage releases without being at the desktop". The
dialog is now one front end of `MainWindow::cutRelease`
(`desktop/src/MainWindowReleases.cpp`), and two more front ends call it
without any window.

## What a cut does

`cutRelease` takes a `forkmesh::releasedraft::Cut` (`desktop/src/ReleaseDraft.h`)
and, on a worker thread:

1. refuses a tag that already exists or a target that does not resolve, before
   anything is written;
2. stamps the version into every versioned file on the target branch
   (`stampReleaseTarget`), then creates the annotated tag;
3. back on the GUI thread: announces the release on the fediverse, optionally
   opens the Mastodon composer, remembers the artifact prune, prunes old tags,
   remembers the mirror rollout and queues the release workflows, exactly as the
   dialog did.

The dialog, the MCP tools and the phone all share this code; only the defaults
differ. `forkmesh::releasedraft` also holds the defaults the dialog always
suggested: the next tag (the last number of the newest tag plus one), the next
title (the previous title with the new tag), and the generated notes (the
commit list since the previous tag, the Generate release notes button).

## MCP tools

All three go over the agent-intake socket ([mcp-create-agent.md](mcp-create-agent.md)),
so they work only inside an agent the desktop launched, and the desktop
re-validates every field (`forkmesh::agentintake::parseReleaseRequest`).

| Tool | Socket method | Does |
| --- | --- | --- |
| `list_releases` | `releases.list` | Newest tags (commit, date, title, and whether a rollout, publication or artifact prune is still pending), the default branch, the next tag and title, and the tag of `.forkmesh/releases/latest/release.json` on the default branch. Read-only. |
| `create_release` | `releases.create` | A cut. `tag` defaults to the next tag, `target` to the default branch, `title` to the next title, `notes` to the generated notes, `rollout` to on when the repository can roll out (the `forkmesh/forkmesh` source checkout with Actions enabled) and the tag is a plain `vX.Y.Z`. Returns the tag, previous tag, target, title, whether notes were generated, whether a rollout was scheduled, and a one-line detail. |
| `publish_release` | `releases.publish` | The Push to mirrors button for an existing tag (`pushReleaseToMirrors(index, tag)`); answers once the push has started. |

`create_release` and `publish_release` are in `WRITE_TOOLS` (a published
connector needs its token) and share a limit of 6 per 10 minutes per desktop
run. Only one cut runs at a time (`m_newReleaseRunning`, shared with the
dialog); a second gets `busy`. The remote path never switches the repository
the desktop window is showing. Refusal codes: `no_checkout`, `bad_tag`,
`tag_exists`, `bad_target`, `rollout_unavailable`, `busy`, `not_created`,
`no_such_release`, `private_repository`, `rate_limited`, `unauthorized`.

Tag pruning and deleting are not offered remotely: both are irreversible, and
the dialog still asks for them.

## Phone requests

`release.create` and `release.publish` from the phone
([../app/desktop-commands.md](../app/desktop-commands.md)) run the same code
with the same defaults (the v1 signature covers only the tag). When the
request is signed by a key the desktop already knows (this desktop, the
account key, a paired phone, a device trusted earlier) they run as soon as the
desktop drains them, with no approval prompt, and the desktop shows a system
notification. Settings → Pings → "Create and publish releases my phone
asks for without asking here" (`desktopCommands/releasesWithoutPrompt`, on by
default) turns that off. Unknown keys still get the prompt; once approved they
also run without the dialog. `release.draft` and `release.notes` still open the
pre-filled dialog, because that is what they ask for.

Creating a release from the phone for a tag that already exists settles as
done ("release already exists"), so pressing Create release on an existing
card is harmless.

## From a terminal (2026-10-12)

A Claude session the desktop did not launch has no intake socket, so the MCP
tools above refuse. `tools/forkmesh_release_request.py` sends the phone's
request instead: it signs `release.create` or `release.publish` with this
device's identity key (`identity/ed25519.pem`, the key the desktop lists as
"This desktop") as the signed-in account, and posts it to
`/api/repo/<account>/<repo>/desktop-commands`. The desktop runs it unattended
like a phone request ([../app/desktop-commands.md](../app/desktop-commands.md)).

```
python3 tools/forkmesh_release_request.py create            # next vX.Y.Z
python3 tools/forkmesh_release_request.py create --tag v1.3.0
python3 tools/forkmesh_release_request.py publish --tag v1.2.8
python3 tools/forkmesh_release_request.py create --dry-run  # print, do not send
```

Only the tag travels (the v1 signature covers nothing else), so title, notes,
target and rollout take the dialog's defaults. The URL owner is the account,
not the org alias: `forkmesh/forkmesh` is posted as `jett/forkmesh`, because
the relay rewrites an org owner to its backing account before checking that
`ownerAccount` matches (403 `not_authorized` otherwise). Before this date the
desktop never drained requests for an org-aliased repository; see the next
section's linked page.

## Activating it

Rebuild and restart the desktop, then start (or restart) the agent that will
call the tools from the desktop so its MCP server inherits the intake socket.
