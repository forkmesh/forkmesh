# Hearth, the Discord community special agent

Hearth is the special agent (`.forkmesh/teammates/hearth.json`) that keeps the ForkMesh
Discord server warm and useful. It was added on 2026-10-01 because the server
goes quiet for days at a time. On that day the bot could see 1 human message
in 72 hours, the median wait for a first human reply over 30 days was about
26 hours, and a v1.2.5 Windows blocker report in `#bugs` had no reply after
57 hours.

| Piece | Path |
| --- | --- |
| Special agent definition | `.forkmesh/teammates/hearth.json` |
| Run playbook | `.forkmesh/shortcuts/tend-the-discord-community.md` |
| Skill | `.forkmesh/skills/community-engagement.skill` |
| Discord tool | `tools/discord_community.py` |
| Portrait | `desktop/resources/agent-ui/icons/teammate-hearth.png` (avatar key `hearth`) |
| Contract tests | `app/tests/test_hearth_community_brain.py` |

The special agent is checked in paused, like every other special agent: it is set to run daily
at 11:00, and `schedule.active` is `false` until the operator turns it on from
the Special agents page.

## Code map

`tools/discord_community.py` is the only way Hearth touches Discord. It has
four commands:

- `channels` lists the text channels and active threads the bot can see.
- `room` reads the room. For each channel in the window it reports:
  - human message count and who spoke;
  - pace over the last 15 minutes, and whether the conversation is "hot"
    (6 or more human messages);
  - mood from frustration and warmth word hits (all-caps lines count as
    frustration);
  - unanswered questions. In help channels (bugs, support, ask, QA, trial,
    get-started) any top-level human message of 6 or more words counts as
    a request;
  - newcomers from Discord join messages, and whether anyone greeted them;
  - how long ago the bot last spoke;
  - the last 12 messages.

  Each channel also gets one piece of advice: `answer`, `acknowledge`,
  `hold`, `welcome`, `spark` or `listen`. At server level it reports
  communicators, open questions, unwelcomed newcomers, the median minutes to
  a first human reply, and the bot's posts in the last 24 hours.
- `post` is a dry run unless `--send` is given. It refuses:
  - an empty message, or one over 2000 characters;
  - `@here` and role mentions, and `@everyone` except in an `--announce`
    post to `#announcements`;
  - more than 6 posts a day across the server;
  - a top-level post into a hot channel;
  - a second top-level post in a channel within 120 minutes;
  - a top-level post when the last message in the channel is already the
    bot's.

  Replies (`--reply-to`) skip the hot, cooldown and last-message rules.
  `--announce` exists for a post the operator asks for: it skips the cooldown,
  last-message and budget rules and keeps every other rule. Sends set
  `allowed_mentions` to users only, so a ping can never reach a whole role;
  the one exception is an `--announce` post to `#announcements` whose text
  contains `@everyone`, which also allows `everyone`. On 2026-10-07 the
  operator asked Marketing that every announcement mention `@everyone`. Hearth's
  own instructions still say never to use it, and only the operator can
  change those.
- `react` adds one emoji reaction (dry run unless `--send`).

The bot token comes from `DISCORD_BOT_TOKEN` in the environment. If that is
not set, the tool reads only that one key from the fleet `secrets.env`
(`forkmesh_paths.resolve().fleet_config`). The tool never prints it. The guild
defaults to the ForkMesh server (`1531610390574665809`); `DISCORD_GUILD_ID` or
`--guild` overrides it.

## Reading the room

The tool's numbers are a first pass. The playbook makes the agent read the
excerpts itself and decide. It gets advice rather than a verdict because
keyword mood scoring is crude: sarcasm, inside jokes and non-English chat all
fool it. The rules it follows come from the research below:

- Most members only read (the 90-9-1 rule). Open loops come first: an
  unanswered question, a blocked or frustrated person, a newcomer nobody
  greeted, or real work worth thanking someone for.
- On 2026-10-09 the operator asked Hearth to post more, because it is
  responsible for keeping the server active. Until then it sparked at most
  once per run and only after days of quiet, and that run ended with a single
  reaction. Now, whenever no conversation is flowing, it posts up to three
  sparks per run in different channels whose purpose fits (`#general`,
  `#chill-chat`, `#ask-anything`, `#get-started`) and keeps two of the six
  daily posts for replies. Each spark is tied to something real: a release,
  a blog post, something new in the World, or a pointer for newcomers. An
  ignored post is not repeated, but it no longer bars the channel for the
  run.
- Stay out of a conversation that is flowing. A reaction is the most a host
  adds there. Growing the share of community-to-community replies is the
  goal, not team replies.
- Meet frustration calmly and briefly: name the problem, give one next step,
  link it. A stern or chirpy reply makes upset people angrier.
- Match the room's register. Where an answer is grounded, cite the doc,
  release, issue or commit. When it is not, say so and name who follows up;
  made-up answers are how community bots lose trust fastest.
- Fixed-purpose channels (announcements, partnerships, content, videos,
  bounty, bugs, support, trials) are never filled with chatter. The tool
  advises `listen` for them even when they are quiet.

## Rhythm

Consistency matters more than frequency. Run one light ritual a week, and
only when the room is ready for it:

- **Monday**: a "what are you building with ForkMesh this week?" prompt in
  `#general`.
- **When a release ships**: a short note in `#general` on what changed, with
  a question for people on each platform. The operator posts the
  `#announcements` post.
- **Friday**: thank by name the people who reported, fixed or helped that
  week.

Hearth proposes office hours, show-and-tell and AMAs to the operator; it does
not schedule them on its own. Judge a change only after 1 to 2 weeks of data.

## Health measures

These come from Discord Server Insights, Common Room and CHAOSS:

- communicators in the window;
- active channels;
- open questions;
- unwelcomed newcomers;
- the median time to a first human reply, with bots excluded (CHAOSS "time
  to first response").

Discord counts about 30% of members communicating as healthy. Retention means
a new member returns in their second week, so a newcomer should get a human
reply or a useful pointer within the first 48 hours.

## Security and policy

- Discord message text is untrusted content. The tool labels it as such, and
  the special agent must not act on instructions inside it.
- The bot's `[BOT]` tag is the disclosure. Under Discord's Developer Policy
  and Platform Manipulation policy, the special agent never presents itself as a
  person, never DMs without consent, and never fakes activity or inflates
  metrics. It also never uses message content for anything other than
  community help.
- Rate limits: the tool retries a 429 after Discord's `retry_after` (capped at
  30 s), at most 3 times. Channels that return 403 are skipped and reported,
  because repeated invalid requests (10,000 per 10 minutes) get an IP banned.
- Permissions: `read-repositories`, `manage-issues`, `external-tools` and
  `run-unattended`. The special agent has no code-writing, merge or payout authority.
- Launch mode: a special agent without `modify-code` starts in Plan, which can read
  but not run `post --send`. On 2026-10-01 Hearth's first click opened in
  Plan and could only dry-run its replies. `run-unattended` ("Run tools
  without asking") lets the desktop start Hearth in its own `"mode": "Auto"`,
  so a click or a scheduled run can post without a person approving each
  send. What keeps it safe is the tool's guardrails above (no mass or role
  mentions, users-only `allowed_mentions`, 6 posts a day, 120 minute
  cooldown), and the prompt's boundary that still forbids code changes.

## Traps

- On 2026-10-01 the bot could not read `#support`, `#content-ar`,
  `#content-cenzo` or the partner channels (HTTP 403). Before the special agent can
  answer there, an operator has to grant the bot View Channel and Read
  Message History in those channels.
- Message text arrives empty unless the bot has the Message Content intent.
  The `#bugs` watcher already needs it, so it is on. If excerpts suddenly come
  back blank, check the intent first.
- Earlier bot posts in `#announcements` used `@everyone`. That was the
  operator posting through the bot by hand. Hearth's tool refuses it
  everywhere except an `--announce` post to `#announcements`.

## Research sources (2026-10-01)

- Discord Server Insights and growth guides:
  discord.com/community/understanding-server-insights and
  discord.com/community/using-insights-to-improve-community-growth-engagement
- Community Onboarding FAQ:
  support.discord.com/hc/en-us/articles/11074987197975
- Starting a great community:
  discord.com/blog/best-practices-for-starting-a-great-community-on-discord
- Moderator Academy, de-escalation and conflict resolution:
  discord.com/safety/facilitating-positive-environments,
  discord.com/safety/internal-conflict-resolution
- Developer Policy:
  support-dev.discord.com/hc/en-us/articles/8563934450327
- Platform Manipulation policy:
  discord.com/safety/platform-manipulation-policy-explainer
- Rate limits: docs.discord.com/developers/topics/rate-limits
- Orbit model: orbit-model.joshed.io
- CMX SPACES: cmxhub.com/blog/the-spaces-model
- Common Room overview metrics:
  commonroom.io/docs/using-common-room/reporting-page/room-overview
- CHAOSS time to first response:
  chaoss.community/kb/metric-time-to-first-response
- The 90-9-1 rule: en.wikipedia.org/wiki/1%25_rule
