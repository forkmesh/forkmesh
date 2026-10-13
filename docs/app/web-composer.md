# Web prompt composer

Every signed-in dashboard page on app.forkmesh.com has a prompt dock modelled
on the desktop app's prompt overlay (`MainWindow::buildNetworkLogDock()` in
`desktop/src/MainWindowChat.cpp`). It lets the user start an agent, add a
follow-up to a session, or file an organization task from any page.

## Code map

| Path | Responsibility |
| --- | --- |
| `app/public/dashboard-composer.js` | The dock: markup, state, requests, slash menu, dictation, attachments. A standalone classic script (not part of the `dashboard.js` bundle), exposing `window.ForkMeshComposer` (`open`, `close`, `toggle`, `insert`, `registerPageHandler`, `state`, `_internals`) and firing `forkmesh:composer-ready` once it is defined |
| `app/public/dashboard-composer.css` | Hand-written styles on the dashboard's `--dashboard-*-rgb` theme tokens, so dark and light follow the page and no Tailwind rebuild is needed |
| `app/public/dashboard/js/10-boot.js` | `window.ForkMeshComposerHost`: the bundle's read-only bridge (session, current page and repository, the agent modal's repository choices, the open agent session, `agentsChanged` to reload the Agents tab). The bundle fires `forkmesh:composer-host` once boot has restored the session; the dock mounts on it if it was not mounted at `DOMContentLoaded` |
| `app/public/dashboard/shell.html` | Loads the CSS and the script (after `dashboard.js`) on every composed page |
| `app/public/dashboard/{tasks,notes,calendar}/index.html` | Hand-maintained page shells; they carry the same two tags by hand |
| `app/src/dashboard_shell.py` | Both files are in `CACHE_BUSTED_ASSETS` |
| `tools/mirror_gateway.py` | Both files are in `DASHBOARD_ASSET_ROUTES` for mirror-served dashboards |
| `app/tests/test_dashboard_composer.py` | Wiring, server-limit parity (models, prompt length, attachment and task limits) and the pure helpers run in Node |
| `app/browser_tests/tests/dashboard-global-composer.spec.js` | Repo issue page (new on the desktop and in the cloud, add, task, slash menu, draft across pages), light theme off repo pages, phone bottom sheet with 44px targets. Set `COMPOSER_SCREENSHOT_DIR` to save screenshots |

## Controls and what they do

| Control | Desktop meaning | Web behaviour |
| --- | --- | --- |
| Pill (collapsed) | Avatar collapses the overlay | "Prompt" pill bottom-right; a green dot when a draft is kept. `Ctrl+/` (`⌘/`) opens the dock and focuses the box; pressed again while typing, it collapses. Opening the dock, by any route, puts the cursor at the end of the text; a dock left open is restored on the next page load with the cursor in the box, unless the page has already focused something else |
| × | Clear the text (undoable) | Same: clears through `execCommand("delete")`, so `Ctrl+Z` restores it |
| Green dot | New agent gets its own worktree | Web jobs always run on their own branch, so the dot is `createPr`: open a pull request when the run finishes. Off by default and turned off again after each start, so every pull request is asked for on purpose; see [Pull requests are explicit](../agents/README.md#pull-requests-are-explicit) |
| Drag handle | Move the overlay; double-click resets | Same on screens 640px and wider; position remembered |
| Counter | Characters left of 16000 | Characters left of the relay's 8000 (`ORG_AGENT_MAX_PROMPT`), counting the context line and inlined text files; amber at 20, red when over |
| Refresh | Reset size and position | Reset position and size, reload app commands, sessions and nodes |
| Expand | Detach into its own window | Toggle a wider, taller dock. The dock opens compact, the size of the desktop overlay (at most 560px wide, a four-line box); expanding is remembered (`large`), and Refresh and double-clicking the handle shrink it again. From 2026-09-27 the dock briefly opened expanded, which took most of a phone screen; the old `expanded` key is ignored so everyone starts compact again |
| Chevron | (none) | Collapse to the pill (also `Esc` in the box, or the avatar) |
| context chip | (none) | The page's repository, issue or pull request; × removes it for this page and shows a repository picker (the agent modal's choices plus recent repositories) |
| Cloud | New agent runs on a ForkMesh agent node instead of this desktop | Same choice, default off, remembered (also in the `/` menu). Off: "new" goes to a desktop node (targets whose `runtimeMode` is `desktop`, or none given, as on the organization route). On: it goes to a cloud node, a managed `headless` node or the `runner-pool` target. With no eligible node on the chosen side the dock says so and sends nothing, instead of letting the relay pick the other side |
| session / on | (none) | The session "add" targets (an unlabelled picker: the visible "add to" label was removed to keep the prompt row short; shown only while a session of the page's repository is open on its Agents tab, since off a session it read as adding a new agent to a session), and the node a new agent runs on, from the side the cloud button picks (shown only with two or more eligible nodes) |
| task | `POST /api/tasks` | Same body: title from the first line (160 chars), details (4000 chars), department `general`, assignee `unassigned`, priority 50, the repository, images (4 × 256 KB, 1 MB total, recompressed to fit) |
| add | Follow-up to the open session | `POST <agent endpoint>/<session id>` (a `steer` job) with prompt and images; the target is the selected session (default: the session open on the Agents tab). Shown, like the session picker, only while a promptable session is open on the repository's Agents tab |
| new | Start a new agent | `POST <agent endpoint>` with `prompt`, `provider` (the model's agent, or `auto`), `model`, `mode` (permission mode), `effort` (speed), `targetNode`, `createPr`, `attachments` |
| page action (e.g. log) | (none) | Shown only while a page handler is active (see below); the prompt and image attachments go to that page instead of an agent |
| Enter | "add" when a transcript is open, else "new" | The active page handler first, if any; otherwise the same rule: "add" when the repository's Agents tab has a session open; the chosen button is outlined. `Shift+Enter` adds a line; Up/Down recall the last 50 prompts |
| Avatar | Collapse | Collapse |
| Paperclip | Images and video | Images (JPEG, PNG, WebP, GIF; others are converted to JPEG) up to 5 per prompt, 5 MB each, 10 MB total (the relay's limits), recompressed when larger; text files up to 64 KB are inlined into the prompt as fenced blocks. Paste and drag-and-drop work too |
| Mic | Hold-to-talk, local whisper.cpp | Click to toggle the browser's Web Speech API; disabled where it is missing |
| `/` | Slash popup: context, model and CLI commands | Filterable popup: attach, insert page context, clear, rewind, switch model, switch permission mode, switch speed, toggle pull request, `/apps`, and the viewer's installed ForkMesh Apps commands (`GET /api/apps/commands`). Typing `/` in an empty box opens it too. Sending `/<app command>` runs it through `/chat-apps.js` and shows the ephemeral reply in the dock. Built-ins typed as text: `/clear`, `/help`, `/apps`, `/model <alias>`, `/mode <auto|ask|plan|edit>`, `/speed <low|medium|high|ultra|max>`, `/agent <auto|claude|codex|gemma>` |
| Model | Model list across agents, with the model faces | One popup grouped Claude Code / Codex / Gemma, each model with the desktop's face icon (models without one show a letter). The relay's `ORG_AGENT_MODEL_ALIASES`, narrowed to the `models` map the agent endpoint returns. Picking a model picks its agent; until one is picked the agent is `auto` (the relay's general bot: Claude Code first, then Codex). Default Opus 5.5; remembered per agent |
| Mode | Permission mode (Auto/Ask/Plan/Edit) | Same four modes and icons, each with what it permits. Default Auto. Sent as `mode` |
| Speed | Reasoning effort (speedometer icon) | Low/Medium/High/Ultra/Max with the desktop's speedometer icons, icon-only when closed. Default High. Sent as `effort` |
| Gauge | Claude Code and Codex account usage windows | Context use of the selected session (`agentInfo.contextTokens / contextWindow`), five bars, empty when no session is open; green, amber at 70%, red at 90% |

The agent endpoint is `/api/repo/<owner>/<repo>/agent-sessions` when the
account owns the repository (the node or one of its linked nodes), else the
organization route `/api/orgs/<org>/repos/<repo>/agent-bots`, as in the header
"Start an agent" modal. Both take the session token in the body as well as the
`Authorization` header. The issue or pull request context travels as a
"Context:" line in the prompt: the owner route rejects `issueNumber` without an
organization task key.

### The "Start an agent" modal

Since 2026-09-27 the header modal picks the agent and the model in one
"Agent and model" select, like the desktop composer's
`quickAddAgentModelSelector`, instead of an Agent select beside a Model select.
`renderAgentModalAgentModels()` in `app/public/dashboard/js/06-repo-content.js`
fills it with one option group per `AGENT_PROVIDER_OPTIONS` entry, holding that
agent's `AGENT_NEW_MODEL_OPTIONS` (the empty "Provider default" becomes
"<agent> · default model", so the closed select still names the agent). Option
values are `<provider>|<model>`; `syncAgentModalAgentModel()` splits the value
into the hidden `provider` and `model` inputs, so the submitted body and the
provider-filtered "Run on" list and sign-in button are unchanged. The modal
markup is copied into every page shell under `app/public` that carries the
header; change all copies together.

Since 2026-10-02 the start form on a repository's Agents tab works the same
way: `renderOrgAgentStartChoices()` fills one "Agent and model" select
(`data-org-agent-agent-model`) through the shared `agentModelChoicesHtml()`,
grouped per provider the agent-bots payload lists and holding that provider's
`payload.models` aliases, and writes the split value into the hidden
`data-org-agent-provider` / `data-org-agent-model` inputs that the submit
handler reads. `splitAgentModelChoice()` is the shared `<provider>|<model>`
parser for both forms.

Since 2026-10-04 the modal sends its pasted or attached screenshots.
`addAgentModalImage()` had queued them on `form._pendingAgentImages` (adhoc
#78), but `handleRepoAgentNewSubmit()` posted only the prompt, so the relay,
the runner (`materializeAttachments` in `server/agent_attachments.go`) and the
agent never saw them: an agent asked to "fix this" over a screenshot got the
words and no picture. `agentModalAttachmentsPayload()` now turns each queued
data URL into the `{mediaType, data, name}` entry `_org_agent_parse_attachments`
expects, and a successful start clears the queue.

Since 2026-10-06 the modal also has the controls for it. The header markup had
never carried the `data-repo-agent-new-file-input`, `data-repo-agent-new-attach`
and `data-repo-agent-new-attachments` elements that `wireAgentModalImages()`
looks for, so the only way to attach was pasting into the task box and the
queued images were never shown. The form now has an **Attach images** button
(JPEG, PNG, WebP or GIF, the types `_org_agent_parse_attachments` accepts;
other image types are refused in the browser) and a thumbnail list with a
remove button per image, hidden while it is empty.

### Errors: Copy and Send to prompt

Since 2026-10-04 every error on a dashboard page has a **Copy** button and a
**Send to prompt** button, like the desktop's top message
(`m_topMessageCopy` / `m_topMessageSendToPrompt`). One `MutationObserver`
installed from `bootDashboard()` (`installErrorActions()` in
`app/public/dashboard/js/06-repo-content.js`) adds the pair inside each error
element, so no error site has to opt in and new ones are covered. An error
element is:

- a `<p>` or `<div>` with `text-destructive`, or a `text-destructive` element
  with a `data-*hint|status|error|notice|warning|message` attribute (the hint
  and status lines the forms recolour). Red `<span>` labels such as diff counts
  ("-12"), "Conflicts" and status words are not errors and get no buttons;
- any `role="alert"` element;
- the composer's status line when its `data-tone` is `bad`;
- anything marked `data-error-copy`.

Elements inside buttons, links or form controls, or that contain a control,
are skipped (`data-error-actions-skip` opts out explicitly); only the
outermost of nested error elements gets the pair, and the pair is removed when
the element stops being an error (a hint recoloured to muted text). Copy puts
the error text on the clipboard. Send to prompt inserts "Fix this error shown
on <path>:" and the text into the composer (`ForkMeshComposer.insert`), or
into the "Start an agent" modal's prompt where the composer is absent.

The error pop-ups that used `window.alert()` (issue actions, post delete,
agent session delete, sign out all devices, status update) now call
`showErrorNotice()`, a dismissible notice in the corner whose text is a
`role="alert"` element and so gets the same buttons. The agent follow-up form
showed its error through `setCustomValidity`, a browser tooltip that cannot be
selected; it now writes a `data-org-agent-followup-error` line under the form.

The built CSS is purged (`tailwind.dashboard.config.js`), so the buttons use
only classes already in `app/public/dashboard/tailwind.css`, and the notice is
placed with inline styles.

### Page handlers

A dashboard page can take over the dock while it is on screen by calling
`window.ForkMeshComposer.registerPageHandler({ id, label, title, placeholder,
busyMessage, emptyMessage, isActive, submit })`. If the composer script has not
loaded yet, the page waits for the `forkmesh:composer-ready` window event. While
`isActive()` returns true the dock shows a primary button with `label`, swaps
the placeholder, and Enter calls `submit({ text, images })` (each image is
`{ name, mediaType, dataUrl }`) in place of "add" or "new". `submit` resolves to
`{ status, tone, message, link, keepPrompt }` for the status line and output
box, or throws an `Error` whose message is shown in red. Slash commands still
run first. Registering again with the same `id` replaces the old handler; the
return value unregisters it. `isActive` is checked again on each Enter and
whenever the dock refreshes its context, so a handler whose page has been
unmounted or hidden stops applying without being unregistered. Calorie Counter
uses this so a prompt typed on its page is logged to the diary.

## State

One `localStorage` key per account, `forkmesh.composer.v1:<account>`: draft
(saved 250 ms after typing stops), open or collapsed, expanded, position,
repository picked off repo pages, agent (`agent`; older keys stored it as
`mode`, which is still read), permission mode (`runMode`), speed (`effort`),
model per agent, `createPr` (reset to off after each start), `cloud`, and prompt history. Attachments are not kept across page loads. Nothing polls:
(saved 250 ms after typing stops), open or collapsed, expanded (`large`), position,
repository picked off repo pages, mode, model per provider, `createPr`, and
prompt history. Attachments are not kept across page loads. Nothing polls:
sessions and nodes load when the dock opens, when the repository changes,
after a send, and on refresh, with a 60 second reuse window.

The dock does not mount for guests, inside iframes, or in the World chat
embed (`html[data-world-embed="1"]`).

## Layout

- Desktop: a 30rem panel pinned bottom-right (46rem expanded), z-index 45,
  below the dashboard's modals and pop-overs (z-50).
- Phones (under 640px): a full-width bottom sheet with safe-area padding; the
  pill shows only its icon. The chat page lifts the pill 12px above its
  composer: `applyPillLift` measures `[data-dashboard-chat-composer]` (a
  `ResizeObserver` plus window resizes) into `--fmc-pill-lift` on the root,
  since the composer's height changes with its wrapped toolbar, task routing
  and resized input. 176px is the fallback before the first measurement.
- Page clearance: while the dock is mounted (`body:has(> .fmc-root)`), every
  dashboard view (`#content > .view`) gets `--fmc-page-clearance` (80px plus
  the bottom safe area) of bottom padding, so the fixed pill never covers the
  last buttons or fields of a page. Full-height views (notes, calendar) end
  their panes above the pill. Home pads its content column (the right rail
  when stacked under 1024px) instead of the section, so the left rail's
  background still reaches the bottom; chat pads `#fullChatMessages`, whose
  end sits under the lifted pill.
  The prompt takes the full width: task/add/new sit in a row under it instead of
  a column beside it, the repository, session and node selects share the meta
  row under the context chip, the model picker takes the toolbar's spare width
  beside the mode and icon-only speed pickers, all without their chevrons (the avatar, a
  second collapse button, is dropped there). Selects use 16px
  text so iOS does not zoom on focus. `applyViewport` follows `visualViewport` and sets `--fmc-keyboard`
  and `--fmc-viewport` on the root, so the sheet sits on top of the on-screen
  keyboard (iOS keeps the layout viewport behind it) and never grows past the
  visible area; expanded, the sheet fills that height and the prompt stretches.
  Pinch zoom turns the tracking off.
- Coarse pointers on wider screens keep the 76px action column.
- Coarse pointers: every control is at least 44px, matching the shell's rule.
- Inline: a page that carries a `[data-composer-slot]` element gets the dock
  inside that element instead of pinned to the corner (`data-inline="1"` on the
  root). It is always open: collapse, `Esc` and the avatar do nothing, the pill,
  the collapse button and the drag grip are hidden, the stored position is
  ignored and the `open` flag is not written, so the floating dock on other
  pages keeps its own state. Draft, mode, model and history are shared with the
  floating dock. Only the dashboard home page has a slot. The page-clearance
  rules do not match there (the root is not a direct child of `body`), as home
  has no pill to clear.

## Gaps against the desktop

- The permission mode only reaches the desktop for owner-route launches; the
  relay drops it for organization launches, whose runs keep the desktop's own
  mode, so an organization member cannot run agents without permission prompts
  on somebody else's desktop. Speed applies on both routes.
- The headless cloud runner (`server/agents.go`) ignores mode and speed: it
  runs Claude Code with `--dangerously-skip-permissions` and its default effort.
- No worktree/direct-checkout choice; the green dot controls the pull request.
- No local transcription (whisper.cpp or Parakeet); dictation depends on the
  browser's speech service.
- Video and non-text files cannot be sent; the relay accepts images only.
- No Claude CLI slash commands (the desktop asks the local CLI); they still
  work when typed, as they go to the agent as text.
- No "Mention file from this project", "Send issue context to agent" as a
  separate step (the context chip does it), thinking toggle, or account usage
  windows.
- The counter limit is 8000, the relay's, not the desktop's 16000.
- "add" on a cloud or desktop session the relay does not know about is not
  possible from the dock; only website-queued sessions are listed there. The
  Agents tab reaches the desktop's own sessions directly
  ([desktop-agents-web.md](desktop-agents-web.md)).
- The cloud button splits targets by the `runtimeMode` the owner route
  returns. The organization route returns none, so its nodes count as
  desktops; cloud targets on an organization repository come from the owner
  route, and only for an account that owns the organization or the backing
  node (see the 2026-09-29 change).

## Dashboard home

The home page (`app/public/dashboard/partials/views/home.html`) is the prompt
and the account's agents; everything else sits in the rails.

- Centre: the inline dock, then "Agents": the newest ten sessions from
  `GET /api/account/agent-sessions` (one indexed read of the account's own and
  its organizations' cloud sessions), each linking to its repository's Agents
  tab, with the running count in the heading. The list reloads on the account
  socket's `cloud-agents` topics (`forkmesh:cloud-agents`) and when the dock
  queues a run (`ForkMeshComposerHost.agentsChanged`). The old per-repository
  fan-out (`requestRepoAgentsList`) always fails in the browser, so the old
  "Active agent sessions" panel never showed.
- Left rail: "Top repositories", each row with the repository logo, its full
  name (wrapped, not truncated), description and host state; the repository
  cards that used to fill the feed are these rows now. "Find a repository"
  shows only from ten repositories (`HOME_REPO_SEARCH_MIN_REPOSITORIES`). Under
  it, "Feed": pings whose `actor` is someone else (not the session's node,
  email or linked nodes). Pings with no actor (operational alerts, host
  changes) and the account's own actions stay in the bell. The feed filter
  went with the repository cards.
- Right rail: the blog card, unchanged.
- Under 1024px the centre column comes first (`order: -1` in `shell.html`), so
  the prompt is at the top on a phone.
- Removed: the ForkBot issue box (ForkBot stays in chat) and the quick buttons
  under it.
- `app/browser_tests/tests/dashboard-home.spec.js` drives all of the above.

## Changes

- 2026-10-04: the "Start an agent" modal sends its screenshots, and every
  dashboard error gets Copy and Send to prompt buttons (see
  [Errors: Copy and Send to prompt](#errors-copy-and-send-to-prompt)).

- 2026-09-29: cloud on an organization repository (`forkmesh/forkmesh`).
  `forkmesh` is an organization alias, not one of the account's nodes, so the
  dock used the organization route (`/api/orgs/<org>/repos/<repo>/agent-bots`),
  which lists no `runtimeMode` and no runner-pool target. With cloud on, every
  send said "No ForkMesh cloud runner can run this agent on forkmesh/forkmesh"
  even though the desktop's cloud launch works there: the owner route rewrites
  the alias to the backing node (`fmnext-n1`) and lists the runner-pool target
  for an account that owns the organization. For a repository the account
  does not own by name, `loadRepoData` now also reads the owner route
  (`/api/repo/<owner>/<repo>/agent-sessions`). `withOwnerRoute` adds its cloud
  targets (all of its targets when the organization route failed) and its
  sessions, each tagged with that `endpoint`, and "new" and "add" post to the
  endpoint of the chosen target or session. For a member who does not own the
  organization the owner route answers 403, which is ignored, and nothing
  changes.

- 2026-09-28: model, permission mode and speed pickers with the desktop's
  icons. The toolbar's "Auto" select was the agent choice, which read as the
  desktop's permission mode; it is now the Mode picker (Auto/Ask/Plan/Edit),
  the agent follows the model picked in the grouped Model popup, and a Speed
  picker sends the reasoning effort. The icons are 64px WebP copies of
  `desktop/resources/agent-ui/icons/` in `app/public/assets/agent-ui/`. The
  relay puts `mode` (owner route only) and `effort` on the `start` job, and the
  desktop's website-job start passes them to `startAdHocAgentForRepo` as the
  mode and strength overrides; unknown values fall back to the desktop's own.

- 2026-09-27: the dashboard home is the inline dock and the agent list; see
  Dashboard home.
- 2026-09-27: cloud button in the title strip. The owner route
  already lists managed nodes and the runner-pool target next to desktops, and
  a web `targetNode` of `runtimeMode: "runner-pool"` is queued with
  `route: "runner_pool"` (`_org_agent_target_mirror` in `app/src/entry.py`), so
  the change is client-only: `eligibleTargets(targets, mode, cloud)` and
  `pickTarget(targets, mode, cloud)` filter on `CLOUD_RUNTIMES`.
