# Attachments in the agent detail and transcripts (native iOS)

Photos sent with a prompt, and files a transcript names, show in the agent
detail. The code carries no comments; this is where the reasons live.

## Where attachments come from

| Source | Listed by | Bytes from | Shown as |
|---|---|---|---|
| Queued run on the relay (owner or org route) | Each history turn's `attachments: [{id, name, mediaType, size}]` | `GET /api/repo/<owner>/<repo>/agent-sessions/<sid>/attachments/<id>` or `/api/orgs/<org>/repos/<repo>/agent-bots/<sid>/attachments/<id>` (signed in) | Thumbnails on the turn |
| Desktop agent over local control (LAN or relay) | `agents.transcript` answers `attachments: [{id, name, mediaType, size}]` | `agents.attachment {id, attachment, preview?}` answers `{name, mediaType, size, data, preview}`, plus `originalSize, width, height` for a relay preview | A thumbnail strip above the transcript |
| Run on this phone | `AgentRunHandle.eventImages` (the photos that went with each `.userMessage`) | This phone's memory | Thumbnails on the prompt bubble |
| A path a transcript names (`Attached image: <path>`, `Attached video: <path>`, the node's `Attached images (open them with your file-reading tool):` block, an agent message) | `AgentTranscriptAttachments.references(in:)` | Nowhere: the file is on the machine that ran the agent | A row with the name |
| Desktop session synced to the relay | The paired desktop's `agents.transcript` list, when the control center lists the session (see "Desktop sessions synced to the relay") | `agents.attachment`, as for a desktop agent | A thumbnail strip above the transcript; names only when the desktop is out of reach |

The relay keeps a queued run's photos sealed for 7 days and serves them only
to the person who sent them: on the owner route the session row belongs to
the signed-in account; on the organization route only the member who
attached the photo may read it back (other members see the name and size).
A relay without the read-back route answers 404, which the phone shows as
"This relay does not keep photos for transcripts." The server half is
`org_agent_session_attachment_handler` in `app/src/route_handlers.py`,
described in [../agents/README.md](../agents/README.md) (Prompt images on the
transcript).

The desktop names each file with an id derived from a per-process secret, the
session and the path (first 32 hex characters of SHA-256), so no path leaves
the desktop and the phone can only ask for a file that session lists. It sends
pictures only: the file itself when it is at most 2.5 MB and its magic bytes
say JPEG, PNG, GIF or WebP; otherwise a JPEG preview at most 2048 px on its
long side (transparent pictures are laid on white). Files over 40 MB are not
decoded (`too_large`); anything that is not a picture answers `not_an_image`.
An older desktop sends no list, and the phone falls back to the transcript's
own markers as rows.

## Pictures through the relay

Relayed control frames travel in 9000-character chunks, and both ends refuse a
reassembled frame over 1 MB (`RelayControlConnection.maxFrameBytes` on the
phone, `kMaxRelayFrameBytes` on the desktop). A 2.5 MB picture is about 3.4 MB
of base64, so instead of raising those caps the desktop answers a relayed
`agents.attachment` with a JPEG preview of at most 700 KB (about 955 KB of
base64 plus a small envelope, which fits one frame).

- The desktop decides: `LocalControlServer::isRelayed(connectionId)` is true
  for a relay connection, and the phone may also ask with `preview: true`.
  The phone sends `preview: true` whenever its link is the relay, so an older
  desktop still gets a hint it can ignore.
- `LocalControlAttachments::relayPayload` reads the file off the GUI thread,
  decodes it with `QImageReader` (auto-rotated, scaled on read to 1600 px on
  its long side), paints it onto a fresh opaque white image (this drops
  transparency and every text/EXIF key Qt would otherwise write back into the
  JPEG) and encodes JPEG at quality 82, 70, 58, 46, 34 until it fits; when
  none fits it shrinks the long side by a third and tries again, down to
  320 px. Every relayed picture is re-encoded, even a small one, so location
  and camera metadata never cross the relay.
- The answer is `{name, mediaType: "image/jpeg", size, data, preview: true,
  originalSize, width, height}`: `size` is the preview's byte count,
  `originalSize` the file's.
- A file that is not a picture (or does not decode) answers
  `too_large_over_relay` when it is over 700 KB, and `not_an_image` as on the
  LAN when it is smaller. Files over 40 MB are still not decoded (`too_large`).
- On the LAN nothing changes: small pictures go as they are, larger ones as a
  2048 px preview, up to 2.5 MB.

On the phone, `DesktopControlHub.attachmentLoader` (FMControl) builds the
`AgentAttachmentLoader` every agent screen uses when the hub is registered:
it asks through `hub.link()`, so the LAN is used when it is up and the relay
otherwise. `LocalAgentTranscript.attachmentPayload` (FMChat) parses the answer
and refuses a preview over 700 KB. `AgentAttachmentStore` keeps a preview under
`<cacheKey>#preview`, apart from the full picture under `<cacheKey>`:

- a full picture in the cache always wins;
- while the desktop is only reachable through the relay, a cached preview is
  used and nothing is fetched again;
- back on the desktop's network, the full picture is fetched once, stored,
  and the preview dropped.

The full-screen viewer shows "Preview — open on the desktop's network for the
full image." with the preview's size and the original's byte count. The All
agents run screen (`AgentRunScreen`) passes its own hub, so desktop-run photos
now load away from home instead of failing.

## Loading and caching

- `AgentAttachmentStore` (FMApi) is an actor holding bytes in memory with a
  48 MB budget, least recently used out first. An item larger than half the
  budget is handed back but not kept. Concurrent asks for one key share one
  fetch; a failure is not cached. Nothing is written to disk, because private
  photos must not leave plaintext copies; a relaunch reads them again from
  their owner-only source.
- `AgentAttachmentLoader` routes a reference to the relay or the desktop and
  checks every picture before anything decodes it: not empty, at most 5 MB
  (3 MB over local control), and magic bytes of JPEG, PNG, GIF, WebP or HEIC.
- Thumbnails load when they appear (`.task(id:)`), decode detached, and are
  kept decoded in an `NSCache` bounded by pixel cost, so a transcript scrolls
  while pictures arrive.
- The share sheet gets a picture as an image; a file gets a copy in a private
  temporary folder with complete file protection, removed when the sheet
  closes.
- Save to Photos asks for add-only access (`NSPhotoLibraryAddUsageDescription`).

## Photos just sent

- A follow-up shows at once as a "sending" turn with its photos, and the
  composer empties. A failed send puts the text and photos back unless
  something new was typed.
- When the relay lists the turn's photos, they are paired with what was sent
  by order, byte count and type (`AgentAttachmentReconcile`); a pair that does
  not line up is never used, so a wrong picture is never shown under a relay
  id. The bytes are stored under the relay's ids, so the thumbnails never go
  to the network.
- A relay that does not list photos still shows the sender theirs:
  `AgentSentPhotos` remembers them by session and turn text for this run of
  the app (12 sessions), and follows a retry that queues a new session.
- The composer's quick launch seeds the new session the same way before the
  shell opens it.

## A failed run's error shows once

The relay builds a failed run's diagnostic from `agentInfo.lastError`, so the
queued-session screen drew the same text as an amber note and a red error.
`AgentLaunchSession.diagnosticNote` is the diagnostic only when it differs from
`lastError` after trimming. The web got the same fix in production
`fe5a131cb`.

## Tests

- `ForkMeshCore/Tests/FMApiTests/AgentAttachmentsTests.swift`,
  `AgentLaunchDiagnosticNoteTests.swift`
- `ForkMeshCore/Tests/FMChatTests/LocalAgentTranscriptTests.swift`
- `ForkMeshCore/Tests/FMAgentsTests/AgentTranscriptAttachmentTests.swift`
- `ForkMeshCore/Tests/FMApiTests/AgentAttachmentsTests.swift`
  (`AgentAttachmentPreviewTests`: separate preview cache, upgrade on the LAN)
- `ForkMeshCore/Tests/FMControlTests/AgentControlTests.swift`
  (`testDesktopPicturesComeThroughTheRelayAsPreviews`: a 700 KB preview
  through the in-memory relay fits one frame)
- `desktop/tests/test_local_control_attachments.cpp`
  (`forkmesh-local-control-attachments-tests`: relay previews fit 700 KB,
  keep their long side at 1600 px, report sizes, drop metadata, and refuse
  large non-pictures with `too_large_over_relay`)

## Desktop sessions synced to the relay (2026-09-30)

A desktop agent opened from the Agents tab (iOS `AgentSessionDetailScreen`,
Android `AgentSessionDetailScreen`) or from a repository's Agents tab (iOS
`RepoAgentSessionScreen`, Android `feature/code/AgentsTab.kt`) reads the
transcript the desktop pushed to the relay. That text names each picture only
by its `Attached image: <path>` line, so these screens showed a row with the
file name and never the picture, although the desktop's own image strip
showed it.

Now each of these screens also asks the desktop that ran the session for its
list:

- `liveDesktopRow` (iOS `FMControl/AgentControl.swift`, Android
  `control/AgentControl.kt`) finds the session in the control center's rows:
  a desktop row, not an outside CLI, with the same repository name
  (case-insensitive) and local id. This is the same key `AgentLines.merge`
  uses to show a desktop session once. A session the paired desktop does not
  list is never asked for, so a session from another desktop on the account
  cannot pick up this desktop's pictures.
- `desktopAttachments(repoName:sessionId:)` then reads `agents.transcript`
  over the hub (LAN or relay) and returns its `attachments`, which are
  `.desktop(sessionId:)` references the loader can fetch.
- When the desktop is out of reach, or lists nothing, the screen keeps the
  file-name rows. A refresh keeps a list the desktop already sent.

Android loaded desktop pictures only through the LAN service
(`AgentsFeature.localControl`), so away from home the strip showed errors.
`AgentAttachmentCache` now fetches through `DesktopControlHub.attachment`
(`control/DesktopAttachmentFetch.kt`): over the relay it asks with
`preview: true` and keeps the preview under `<cacheKey>#preview`. That entry
is used only while the link is not the LAN. Back on the LAN the full picture
is fetched, stored under `<cacheKey>`, and the preview dropped, as on iOS.
Android also shows "too large to open through the relay" for
`too_large_over_relay`.

Tests: `DesktopSessionAttachmentsTest` (Android core),
`AgentTranscriptAttachmentTests.testADesktopSessionShowsThePicturesTheDesktopLists`,
`testADesktopOutOfReachLeavesTheNamedFiles`,
`AgentControlTests.testTheLiveDesktopRowMatchesRepoAndSession` and
`ReposTabsTests.testAgentSessionDetailShowsTheDesktopsPictures` (iOS).

## The All agents screen

WS7's run screen (`Features/Agents/AllAgents/AgentRunScreen.swift`) shows a run's
photos and files: desktop runs use the list `agents.transcript` sends, node runs
use each relay turn's list, and an older desktop falls back to the file paths the
transcript names. Desktop-run photos load on the desktop's network in full and
through the relay as previews (see "Pictures through the relay").

## Photos in phone prompts to a desktop agent

Until 2026-09-29 `agents.start` and `agents.prompt` took text only. The shell
composer refused photos when the destination was the desktop ("Photos cannot
go straight to the desktop yet"), and the follow-up composers on a desktop
run had no photo button, so a photo attached on the phone never reached the
desktop session. Now every desktop destination carries them, on the LAN and
through the relay.

The phone uploads each photo before the prompt, on the same channel
(`hub.link()` once; staged photos belong to one connection, so a prompt that
went over the LAN cannot name photos sent through the relay):

1. `agents.image.begin {mediaType, size, sha256}` answers `{image}`, a
   32-hex id. JPEG, PNG, GIF and WebP up to 5 MB, five per connection
   (`too_many_images`, `bad_image`).
2. `agents.image.chunk {image, data}` appends at most 192 KB (about 262 KB of
   base64, well inside the relay's 1 MB frame). More bytes than declared drop
   the image (`bad_chunk`).
3. `agents.start {…, images: [id]}` or `agents.prompt {id, text, images: [id]}`.
   The desktop checks every named image (complete, SHA-256, magic bytes), writes
   each as `<data>/agent-images/phone-<uuid>.<ext>` and adds an
   `Attached image: <path>` line per image after the prompt, the same lines a
   pasted image or a relay job's image gets. The CLI reads them, the desktop's
   image strip shows them, and `agents.transcript` lists them back by id, so
   the phone's thumbnails come from the desktop like any other attachment.
   One bad or missing image fails the whole prompt (`unknown_image`,
   `image_incomplete`, `bad_image`, `cannot_save_image`), and every image the
   prompt named leaves the stage either way.

`agents.image.cancel {image}` drops one; a closed connection drops its stage.
An older desktop answers `unsupported` to `agents.image.begin`; the phone says
`images_unsupported` ("Update ForkMesh on the desktop to send it photos.").
The desktop lists `agents.images` in `status.capabilities`.

Through the relay the phone waits after each chunk so its uploads stay near
100,000 base64 characters a second: the relay allows 1000 messages of 9000
characters per 10 seconds per socket (`NODE_CONTROL_MSG_MAX_PER_WINDOW`; 200
before 2026-09-30) and a dropped chunk breaks the channel. A 2 MB photo takes about 30 seconds that way;
on the LAN there is no wait.

A photo stays attached until the send succeeds: the composer empties when the
prompt goes, and a failed send puts the text and photos back unless something
new was typed. The Android run screen now shows the attachments
`agents.transcript` and the relay list, as iOS already did.

Code: `desktop/src/LocalControlPromptImages.*` (staging, tested by
`forkmesh-local-control-prompt-images-tests`), `MainWindowLocalControl.cpp`,
`FMControl/DesktopPromptImages.swift`, `control/DesktopPromptImages.kt`.
Tests: `DesktopPromptImagesTests` (iOS), `DesktopPromptImagesTest` and
`QuickLaunchTest.testPhotosGoStraightToTheDesktop` (Android),
`QuickAgentLaunchTests.testPhotosGoStraightToTheDesktop` (iOS).

## Text files in a prompt (2026-10-04)

Both phone apps upload text files to an agent from the composer's "+" menu
("Upload Text File" on iOS, "Text file" on Android) or `/file`. This covers a
new agent (the shell prompt, the quick start, the start card) and follow-ups
(the agent detail, a runner run, the on-device agent).

The relay's attachment store takes pictures only. The server checks each
upload's magic bytes against `image/*` (`server/agent_attachments.go`,
`ORG_AGENT_ATTACHMENT_*`). An agent on this phone can only read files in its
own worktree, and writing an upload there would leave the tree dirty. So the
file's text goes into the prompt. At send time the composer adds a header and
wraps each file in `===== BEGIN FILE name =====` / `===== END FILE name =====`.
Every send path (relay queue, LAN, desktop, on-device) therefore receives the
file without any change of its own. This matches what the retired Flutter app
did.

Rules (`FMAgents/PromptTextFiles.swift`, `agents/PromptTextFiles.kt`):

- The file must be strict UTF-8 with no NUL. A UTF-8 BOM is dropped and CRLF
  becomes LF. Binaries and UTF-16 are refused with a sentence that points to
  the photo entries.
- At most 32 KB read, and 5 files.
- The folded prompt must fit `maxAgentPromptChars` (8000, the relay's
  `ORG_AGENT_MAX_PROMPT`). This is checked when a file is added and again on
  send, so the relay never cuts an upload short.
- Files show as chips (name and character count) above the field until sent.
  The folded text then replaces the field's text, so a failed send leaves the
  whole prompt visible to resend.

The running-agent count on the Agents tab, the other half of the old Flutter
request, already existed natively: `HomeShell.swift` (`ShellCounts.agentsTotal`)
and `HomeShell.kt` (`agentsSurface.active`).

Tests: `PromptTextFilesTests` (iOS core, Linux) and `PromptTextFilesTest`
(Android core).

## Not covered yet

- Full-size pictures through the relay. Previews cover viewing; the original
  would need both frame caps raised to about 4.5 MB for `agents.attachment`
  answers only, bounded per connection (about 380 relay messages per picture,
  counted against relay quotas).
- Non-picture files through the relay (logs, videos): only their names show.
