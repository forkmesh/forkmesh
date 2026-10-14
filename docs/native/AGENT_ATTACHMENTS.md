# Attachments in agent detail and transcripts (native apps)

Photos sent with a prompt, and files a transcript names, show in the agent
detail on iOS and Android.

## Sources

| Source | Listed by | Bytes from | Shown as |
|---|---|---|---|
| Queued run on the relay (owner or org route) | Each history turn's `attachments: [{id, name, mediaType, size}]` | `GET /api/repo/<owner>/<repo>/agent-sessions/<sid>/attachments/<id>` or `/api/orgs/<org>/repos/<repo>/agent-bots/<sid>/attachments/<id>` | Thumbnails on the turn |
| Desktop agent over local control (LAN or relay) | `agents.transcript` answers `attachments: [...]` | `agents.attachment {id, attachment, preview?}` answers `{name, mediaType, size, data, preview}` (+ `originalSize, width, height` for a preview) | Thumbnail strip above the transcript |
| Run on this phone | `AgentRunHandle.eventImages` | Phone memory | Thumbnails on the prompt bubble |
| A path a transcript names (`Attached image: <path>`, `Attached video: <path>`, the node's attached-images block) | `AgentTranscriptAttachments.references(in:)` | None: the file is on the machine that ran the agent | A row with the name |
| Desktop session synced to the relay | The paired desktop's `agents.transcript`, when the control center lists the session | `agents.attachment` | Thumbnail strip; names only when the desktop is unreachable |

The relay keeps a queued run's photos sealed for 7 days and serves them only to
their sender (owner route: the session's account; org route: the member who
attached them, others see name and size). A relay without the read-back route
answers 404, shown as "This relay does not keep photos for transcripts." Server
side: `org_agent_session_attachment_handler` in `app/src/route_handlers.py`,
see [../agents/README.md](../agents/README.md).

The desktop names each file with an id derived from a per-process secret, the
session and the path (first 32 hex of SHA-256), so no path leaves the desktop
and the phone can only ask for files that session lists. It sends pictures
only: the file itself when at most 2.5 MB and its magic bytes are JPEG, PNG,
GIF or WebP, otherwise a JPEG preview at most 2048 px long side (transparency
on white). Files over 40 MB are not decoded (`too_large`); non-pictures answer
`not_an_image`. A desktop that sends no list makes the phone fall back to the
transcript's markers.

## Pictures through the relay

Relayed control frames travel in 9000-character chunks and both ends refuse a
reassembled frame over 1 MB (`RelayControlConnection.maxFrameBytes`,
`kMaxRelayFrameBytes`). The desktop therefore answers a relayed
`agents.attachment` with a JPEG preview of at most 700 KB.

- The desktop decides with `LocalControlServer::isRelayed(connectionId)`; the
  phone also sends `preview: true` whenever its link is the relay.
- `LocalControlAttachments::relayPayload` decodes off the GUI thread
  (auto-rotated, 1600 px long side), paints onto an opaque white image (drops
  transparency and all EXIF/text keys) and encodes JPEG at quality 82, 70, 58,
  46, 34 until it fits, then shrinks by a third and retries down to 320 px.
  Every relayed picture is re-encoded, so location and camera metadata never
  cross the relay.
- `size` is the preview's byte count, `originalSize` the file's. A non-picture
  over 700 KB answers `too_large_over_relay`.

On the phone, `DesktopControlHub.attachmentLoader` (FMControl; Android
`control/DesktopAttachmentFetch.kt`) asks through `hub.link()`, so the LAN is
used when up and the relay otherwise. A preview over 700 KB is refused.
The cache keeps a preview under `<cacheKey>#preview`, apart from the full
picture under `<cacheKey>`:

- a cached full picture always wins;
- while only the relay is reachable, a cached preview is used and not
  refetched;
- back on the LAN the full picture is fetched once and the preview dropped.

The viewer labels a preview with its size and the original's byte count.

## Loading and caching (iOS)

- `AgentAttachmentStore` (FMApi) is an in-memory actor with a 48 MB LRU budget;
  items larger than half the budget are not kept; concurrent asks share one
  fetch; failures are not cached. Nothing is written to disk.
- `AgentAttachmentLoader` checks every picture before decoding: non-empty, at
  most 5 MB (3 MB over local control), magic bytes of JPEG, PNG, GIF, WebP or
  HEIC.
- Thumbnails load on appear, decode detached and are kept in an `NSCache`
  bounded by pixel cost.
- Sharing a file copies it into a private temporary folder with complete file
  protection, removed when the sheet closes. Save to Photos asks for add-only
  access.

## Photos just sent

- A follow-up shows at once as a "sending" turn with its photos. A failed send
  restores text and photos unless something new was typed.
- When the relay lists the turn's photos, they are paired with what was sent by
  order, byte count and type (`AgentAttachmentReconcile`); a mismatched pair is
  never used. Bytes are stored under the relay's ids.
- `AgentSentPhotos` remembers sent photos per session and turn text for the app
  run (12 sessions), for relays that do not list photos.

## Desktop sessions synced to the relay

Session screens that read the transcript the desktop pushed to the relay also
ask the desktop that ran it for its list:

- `liveDesktopRow` (iOS `FMControl/AgentControl.swift`, Android
  `control/AgentControl.kt`) finds a desktop row (not an outside CLI) with the
  same repository name (case-insensitive) and local id, the key
  `AgentLines.merge` uses. A session the paired desktop does not list is never
  asked for.
- `desktopAttachments(repoName:sessionId:)` reads `agents.transcript` over the
  hub and returns `.desktop(sessionId:)` references.
- Unreachable desktop or empty list: the file-name rows stay.

## Photos in prompts to a desktop agent

`agents.start` and `agents.prompt` carry photos on the LAN and the relay. The
phone stages each photo before the prompt on the same connection (staged
photos belong to one connection):

1. `agents.image.begin {mediaType, size, sha256}` returns `{image}` (32 hex).
   JPEG, PNG, GIF, WebP up to 5 MB, five per connection (`too_many_images`,
   `bad_image`).
2. `agents.image.chunk {image, data}` appends at most 192 KB; extra bytes drop
   the image (`bad_chunk`).
3. `agents.start {..., images: [id]}` or `agents.prompt {id, text, images: [id]}`.
   The desktop verifies each image (complete, SHA-256, magic bytes), writes
   `<data>/agent-images/phone-<uuid>.<ext>` and appends an
   `Attached image: <path>` line per image. One bad image fails the whole prompt
   (`unknown_image`, `image_incomplete`, `bad_image`, `cannot_save_image`);
   every named image leaves the stage either way.

`agents.image.cancel {image}` drops one; a closed connection drops its stage. A
desktop without the feature answers `unsupported`, shown as "Update ForkMesh on
the desktop to send it photos." The capability is `agents.images` in
`status.capabilities`. Over the relay the phone paces chunks to about 100,000
base64 characters per second to stay inside the relay's per-socket message
rate (`NODE_CONTROL_MSG_MAX_PER_WINDOW`).

Code: `desktop/src/LocalControlPromptImages.*`, `MainWindowLocalControl.cpp`,
`FMControl/DesktopPromptImages.swift`, `control/DesktopPromptImages.kt`.

## Text files in a prompt

The composer's "+" menu ("Upload Text File" / "Text file") and `/file` add text
files to any agent prompt. The relay's attachment store takes pictures only, so
the text is folded into the prompt, each file wrapped in
`===== BEGIN FILE name =====` / `===== END FILE name =====`. Every send path
therefore receives files unchanged.

Rules (`FMAgents/PromptTextFiles.swift`, `agents/PromptTextFiles.kt`): strict
UTF-8 without NUL (BOM dropped, CRLF to LF), at most 32 KB per file and 5
files, and the folded prompt must fit `maxAgentPromptChars` (8000, the relay's
`ORG_AGENT_MAX_PROMPT`), checked on add and on send.

## Failed run errors

`AgentLaunchSession.diagnosticNote` is the relay's diagnostic only when it
differs from `lastError` after trimming, so a failure shows once.

## Tests

- iOS: `FMApiTests/AgentAttachmentsTests.swift`,
  `AgentLaunchDiagnosticNoteTests.swift`,
  `FMChatTests/LocalAgentTranscriptTests.swift`,
  `FMAgentsTests/AgentTranscriptAttachmentTests.swift`,
  `FMControlTests/AgentControlTests.swift`, `DesktopPromptImagesTests`,
  `PromptTextFilesTests`.
- Android: `DesktopSessionAttachmentsTest`, `DesktopPromptImagesTest`,
  `PromptTextFilesTest`.
- Desktop: `forkmesh-local-control-attachments-tests`,
  `forkmesh-local-control-prompt-images-tests`.

## Limits

- Full-size pictures and non-picture files do not cross the relay; previews and
  names do.
