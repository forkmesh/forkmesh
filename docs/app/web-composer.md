# Web prompt composer

Every signed-in dashboard page has a prompt dock, modelled on the desktop
prompt overlay, that starts an agent, adds a follow-up to an open session, or
files an organization task.

| Path | Responsibility |
| --- | --- |
| `app/public/dashboard-composer.js` | The dock; exposes `window.ForkMeshComposer` (`open`, `close`, `toggle`, `insert`, `registerPageHandler`, `state`) and fires `forkmesh:composer-ready` |
| `app/public/dashboard-composer.css` | Styles on the `--dashboard-*-rgb` theme tokens |
| `app/public/dashboard/js/10-boot.js` | `window.ForkMeshComposerHost`, the read-only bridge to the dashboard bundle |
| `app/src/dashboard_shell.py`, `tools/mirror_gateway.py` | Cache busting and mirror asset routes |
| `app/tests/test_dashboard_composer.py`, `app/browser_tests/tests/dashboard-global-composer.spec.js` | Wiring, server-limit parity, browser flows |

## Requests

- **new**: `POST <agent endpoint>` with `prompt`, `provider`, `model`, `mode`,
  `effort`, `targetNode`, `createPr`, `attachments`.
- **add**: `POST <agent endpoint>/<session id>` (a `steer` job), only while a
  session is open on the repository's Agents tab.
- **task**: `POST /api/tasks`.
- **Enter**: active page handler, else "add" when a session is open, else
  "new".

The agent endpoint is `/api/repo/<owner>/<repo>/agent-sessions` for the
repository owner, else `/api/orgs/<org>/repos/<repo>/agent-bots` (merged with
the owner route's cloud targets when that answers). Limits follow the relay:
8000-character prompts (`ORG_AGENT_MAX_PROMPT`), up to 5 images of 5 MB
(10 MB total), text files up to 64 KB inlined. The Cloud toggle filters
targets by `CLOUD_RUNTIMES`; with no eligible target nothing is sent.
`createPr` defaults off and resets after each start.

## Page handlers

`registerPageHandler({ id, label, title, placeholder, busyMessage,
emptyMessage, isActive, submit })` lets a page take over Enter while
`isActive()` is true; `submit({ text, images })` resolves to
`{ status, tone, message, link, keepPrompt }` or throws.

## State and layout

State is one `localStorage` key per account, `forkmesh.composer.v1:<account>`
(draft, open state, position, repository, agent, mode, speed, models,
`createPr`, `cloud`, history). Nothing polls. The dock does not mount for
guests, in iframes or in the World embed. On phones it is a bottom sheet that
follows `visualViewport`; pages reserve `--fmc-page-clearance`. A
`[data-composer-slot]` element (the dashboard home) hosts an always-open
inline dock.

`installErrorActions()` adds Copy and Send to prompt buttons to every
dashboard error element; `showErrorNotice()` replaces `window.alert()`. The
"Start an agent" modal uses one `<provider>|<model>` select
(`splitAgentModelChoice`) and is copied into every page shell; change all
copies together.

Not supported on the web: local transcription, video uploads, local CLI slash
commands. See [desktop-agents-web.md](desktop-agents-web.md) and
[agents/README.md](../agents/README.md).
