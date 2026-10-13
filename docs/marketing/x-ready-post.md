# Herald and the ready X post

On 2026-10-08 the operator asked for the X content section to always have a
post ready to go: written by Claude Opus 5.5 from past posts, their replies and
engagement, the current marketing plan and what will get people's attention,
with the text and image there to copy, and a one-tap post from a phone.

| Piece | Path |
| --- | --- |
| Herald definition | `.forkmesh/teammates/herald.json` |
| Herald playbook | `.forkmesh/shortcuts/keep-an-x-post-ready.md` |
| Herald portrait | `desktop/resources/agent-ui/icons/teammate-herald.png` (avatar key `herald`), phone faces `teammate_faces/herald.jpg` and `teammate-face-herald.imageset` |
| Stored ready post, `GET`/`PUT`/`PATCH /ready-post` | `marketplace/packages/marketing/content/worker/marketing.py` ([package README](../../marketplace/packages/marketing/README.md#ready-x-post-1310)) |
| Agent tools `marketing_x_context`, `marketing_save_ready_post` | `tools/forkmesh_mcp_server.py` |
| Contract tests | `app/tests/test_herald_x_post_agent.py`, `marketplace/packages/marketing/tests/test_marketing.py`, `tools/test_forkmesh_mcp_server.py` |

## How the pieces fit

1. **Herald writes.** Herald is a special agent attached to the `x` channel
   (`marketingChannels: ["x"]`). Its playbook reads `marketing_x_context`
   (top and recent X posts with views, likes, replies and reposts, X content
   in the pipeline, the follower trend, open campaigns and the ready post's
   history), `docs/marketing/plan.md`, the campaign briefs and what shipped,
   drafts three candidates, keeps the best, makes a 16:9 image and saves both
   with `marketing_save_ready_post`. Both tools sign as the desktop's account
   with the `forkmesh-marketing-request-v1` proof, the same way the CRM tools
   sign theirs.
2. **The worker keeps one ready post per channel.** It is sealed in
   `marketing_ready_posts` with the last 30 closed ones (posted, skipped,
   replaced), so Herald does not repeat itself and learns from what the team
   skipped.
3. **People post it.** The X channel page on the desktop, the X card on the
   web and the top of the Content section on the phones show the text, the
   image and **Post on X**. On a phone that is the system share sheet with
   the image and the text, so picking X opens its composer with both.
4. **The next one starts.** The post closes when someone presses Mark posted
   or Skip, when the iOS share sheet completes into an app that posts, when
   `POST /x-post` sends the same text, or when the browser extension's X sync
   brings in a new post with the same text. The desktop then starts Herald
   for the next one.

## Choices and why

- **A special agent, not ForkMesh AI.** The package's own AI capability
  (`runtime.ai_json`) runs the host's Workers AI models, so it cannot be
  Opus 5.5, and it cannot draw an image or read the marketing plan in the
  repository. Herald runs in Claude Code on the desktop. Special agents run on
  the model chosen in the prompt settings (`docs/desktop/agents-ui.md`, "No
  prescribed agent settings"), which is Claude Opus 5.5 by default; Herald
  records the model it ran on with each post and the page shows it.
- **Never posts.** A person presses Post. That keeps the account's voice and
  the FTC and platform rules in [README.md](README.md#guardrails-and-why) in
  human hands, and needs no new X write permission.
- **The image lives with the post.** It is a data URL in the sealed row (PNG,
  JPEG or WebP, at most 960,000 characters, about 700 KB), so phones get it
  through the same signed API as the text and nothing is published before
  someone posts it. The overview carries the post without the image; clients
  fetch `GET /ready-post` for it.
- **X intents cannot carry an image.** `intentUrl` fills the text only, so the
  desktop copies the image to the clipboard before opening it, and phones use
  the share sheet instead.
- **Paused when checked in.** Like every special agent, Herald's daily 08:00
  schedule is inactive until the operator turns it on from the Special agents
  page. The playbook stops early when a post from the last 3 days is waiting,
  so a daily run costs little.
