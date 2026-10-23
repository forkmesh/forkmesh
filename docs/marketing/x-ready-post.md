# Herald and the ready X post

The X channel always has a post ready to go, written by a special agent from
past posts and their engagement, open campaigns and the marketing plan, with
an image, for a person to copy or post.

| Piece | Path |
| --- | --- |
| Herald definition | `.forkmesh/workflows/herald.json` |
| Herald playbook | `.forkmesh/shortcuts/keep-an-x-post-ready.md` |
| Herald portrait | `desktop/resources/agent-ui/icons/teammate-herald.png` (avatar key `herald`), phone faces `teammate_faces/herald.jpg` and `teammate-face-herald.imageset` |
| Stored ready post, `GET`/`PUT`/`PATCH /ready-post` | `marketplace/packages/marketing/content/worker/marketing.py` ([package README](../../marketplace/packages/marketing/README.md#ready-x-post)) |
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
   with the `forkmesh-marketing-request-v1` proof.
2. **The worker keeps one ready post per channel**, sealed in
   `marketing_ready_posts` with the last 30 closed ones (posted, skipped,
   replaced), so Herald does not repeat itself.
3. **People post it.** The X channel page on the desktop, the X card on the
   web and the Content section on the phones show the text, the image and
   **Post on X**; on a phone that is the system share sheet.
4. **The next one starts.** The post closes on Mark posted or Skip, a
   completed iOS share, `POST /x-post` with the same text, or a synced X post
   with the same text. The desktop then starts Herald again.

## Rules

- **Runs as a special agent** in Claude Code on the desktop, on the model
  chosen in the prompt settings, because the package's own AI capability
  (`runtime.ai_json`) cannot draw an image or read the repository. Herald
  records the model it ran on with each post.
- **Never posts.** A person presses Post, which keeps the account's voice and
  the rules in [README.md](README.md#guardrails-and-why) in human hands and needs no X
  write permission.
- **The image lives with the post**: a data URL in the sealed row (PNG, JPEG
  or WebP, at most 960,000 characters, about 700 KB), never in the
  repository. The overview omits it; clients fetch `GET /ready-post`.
- **X intents cannot carry an image.** `intentUrl` fills the text only, so the
  desktop copies the image to the clipboard first and phones use the share
  sheet.
- Posts are 280 characters or fewer, counting each link as 23.
- **Paused when checked in.** The daily 08:00 schedule is inactive until the
  operator turns it on. The playbook stops early when a post from the last 3
  days is waiting.
