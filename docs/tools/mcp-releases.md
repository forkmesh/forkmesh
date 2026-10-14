# Releases without the desktop dialog

Part of [the tools docs](README.md); the release pipeline itself is in
[../release/README.md](../release/README.md).

The desktop's "Draft a new release" dialog is one front end of
`MainWindow::cutRelease` (`desktop/src/MainWindowReleases.cpp`). MCP tools,
phone requests and a terminal tool call the same code without any window.

## What a cut does

`cutRelease` takes a `forkmesh::releasedraft::Cut` (`desktop/src/ReleaseDraft.h`)
and, on a worker thread:

1. refuses a tag that already exists or a target that does not resolve;
2. stamps the version into every versioned file on the target branch
   (`stampReleaseTarget`), then creates the annotated tag;
3. back on the GUI thread: announces the release, prunes old tags as
   configured, records the mirror rollout and queues the release workflows.

`forkmesh::releasedraft` holds the defaults: the next tag (last number of the
newest tag plus one), the next title, and generated notes (the commit list
since the previous tag).

## MCP tools

All three go over the agent-intake socket ([mcp-create-agent.md](mcp-create-agent.md)),
so they work only inside an agent the desktop launched; the desktop
re-validates every field (`parseReleaseRequest`).

| Tool | Socket method | Does |
| --- | --- | --- |
| `list_releases` | `releases.list` | Newest tags with pending rollout, publication or prune state; the default branch, next tag and title |
| `create_release` | `releases.create` | A cut; every field defaults as in the dialog |
| `publish_release` | `releases.publish` | Push an existing tag to mirrors (`pushReleaseToMirrors`) |

`create_release` and `publish_release` are in `WRITE_TOOLS` and share a limit
of 6 per 10 minutes per desktop run. One cut runs at a time (`busy`).
Refusal codes: `no_checkout`, `bad_tag`, `tag_exists`, `bad_target`,
`rollout_unavailable`, `busy`, `not_created`, `no_such_release`,
`private_repository`, `rate_limited`, `unauthorized`. Tag pruning and
deleting are not offered remotely.

## Phone requests

`release.create` and `release.publish` from the phone
([../app/desktop-commands.md](../app/desktop-commands.md)) run the same code
with the same defaults (the v1 signature covers only the tag). Requests
signed by a key the desktop already knows run without an approval prompt
and raise a system notification; Settings → Pings → "Create and publish
releases my phone asks for without asking here"
(`desktopCommands/releasesWithoutPrompt`, on by default) turns that off.
`release.draft` and `release.notes` open the pre-filled dialog. Creating an
existing tag settles as done.

## From a terminal

`tools/forkmesh_release_request.py` signs `release.create` or
`release.publish` with this device's identity key as the signed-in account
and posts it to `/api/repo/<account>/<repo>/desktop-commands`:

```
python3 tools/forkmesh_release_request.py create            # next vX.Y.Z
python3 tools/forkmesh_release_request.py create --tag v1.3.0
python3 tools/forkmesh_release_request.py publish --tag v1.2.8
python3 tools/forkmesh_release_request.py create --dry-run  # print, do not send
```

Only the tag travels; other fields take the defaults. The URL owner is the
account, not an organization alias, because the relay checks that
`ownerAccount` matches (403 `not_authorized` otherwise).

## Activating it

Rebuild and restart the desktop, then start the calling agent from the
desktop so its MCP server inherits the intake socket.
