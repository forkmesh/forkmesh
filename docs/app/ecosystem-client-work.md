# Client-work tools from the 2026-10-27 X thread

Analysis behind `tools/ecosystem_client_work.json` and the "Work & client
management" category (`work-management`) of the [Ecosystem](ecosystem.md).

## The thread

[@askwhykartik](https://x.com/askwhykartik/status/2115118935150354782), an
agency owner, posted on 2026-10-27: agencies and freelancers juggle Trello,
Notion, Slack, email and invoicing apps, and would pay monthly for one
platform covering **project tracking, client chat, docs and images, invoicing
and payments**. A day later it had about 37,000 views, 360 likes, 485 bookmarks
and 141 replies.

X shows logged-out readers about 27 replies and its API pages no further, so
this analysis covers those 27 and their sub-threads (read through
`api.fxtwitter.com` on 2026-10-28). The other replies were not reachable;
signing in to X and re-reading the thread would show them.

## What the replies said

- **It already exists.** One reply named Taskip, SuiteDash, Plutio and
  ClientRamp as suites sold as exactly this replacement; another named
  Basecamp, and a third answered that Basecamp has no invoicing or payments.
- **Makers pitched their own.** Loftside, NEXPMX, Daily Tracker, Polynet,
  Biscuit and Ignara were linked or named by their makers or friends.
- **Many built it in-house.** Seven replies said they run their own internal
  system (one in C#, one with an MCP server so the agency's agents share it
  as a single source of truth). None is a public product, so none is listed.
- **Clients will not log in.** One reply argued agencies drift back to Slack
  and Notion because clients will not use another portal, and the wedge is
  syncing an email or WhatsApp thread into the tracker. Loftside's
  magic-link-per-client design answers the same point.
- **Hook it to an AI.** The most-liked reply asked why not connect every
  tool to ChatGPT or Claude over MCP; the answer under it was that one
  platform with built-in MCP costs less than many subscriptions plus AI.
- **Agency software is hard.** One reply called building for agencies "death
  by a thousand scope changes".

## Coverage of the five needs

✓ covered, ~ partly or not yet live, blank not covered.

| Tool | Projects | Client chat | Docs and images | Invoicing | Payments | Also |
| --- | --- | --- | --- | --- | --- | --- |
| Taskip | ✓ | ✓ | ✓ | ✓ | ✓ | CRM, timesheets, WhatsApp inbox, AI agent, MCP server |
| SuiteDash | ✓ | ✓ | ✓ | ✓ | ✓ | CRM, white-label app, courses, email marketing |
| Plutio | ✓ | ✓ | ✓ | ✓ | ✓ | Proposals, contracts, wiki, desktop apps |
| ClientRamp | ✓ | ✓ | ✓ | ✓ | ✓ | Portal embedded on the agency's own site |
| NEXPMX | ✓ | ~ | ✓ | ✓ | ~ | GST invoices, UPI only, AI studio |
| Loftside | ✓ | ✓ | ✓ | ✓ | ~ | Magic links, per-task client visibility; not launched |
| Basecamp | ✓ | ✓ | ✓ | | | Client access, Hill Charts |
| Notion | ✓ | | ✓ | | | Wikis, databases, Notion AI |
| Trello | ✓ | | ~ | | | Power-Ups |
| Daily Tracker | ~ | | | | | Routines, timers, daily score |
| Polynet | | ~ | ~ | | ~ | Paid communities, AI community agent |
| Biscuit | | | | ~ | ~ | Business launch: billing, infrastructure, legal |
| Ignara | ~ | | | | | AI system for one solopreneur |

ClientRamp's row comes from directory listings: clientramp.com did not answer
on 2026-10-28. Slack, ChatGPT (OpenAI), Claude (Anthropic) and MCP were also
named and were already listed, so they are not repeated.

## Each tool and ForkMesh

Levels are the ones stored under `internal` (`overlap`, `threat`,
`opportunity`, `confidence`, `action`).

| Tool | Overlap | Threat | Opportunity | Action | Why |
| --- | --- | --- | --- | --- | --- |
| Taskip | medium | low | medium | investigate | Same tasks, chat and CRM ground as the desktop packages, and it opens its workspace to agents over MCP; its client portal and invoices are the parts ForkMesh lacks. Worth watching how agencies use its MCP server. |
| SuiteDash | medium | low | low | ignore | The broadest suite, white-label first; competes with every SaaS suite, not with Git hosting. |
| Plutio | medium | low | low | ignore | Same shape as SuiteDash with desktop apps; nothing a repository network would adopt. |
| ClientRamp | low | low | low | ignore | A portal on the agency's own site; little public detail. |
| Basecamp | medium | low | low | ignore | Its "project holds everything" model is the nearest to a repository holding issues, projects, discussions and chat; no billing, which the thread wanted. |
| Trello | medium | low | low | ignore | Boards like the Tasks package; the thread names it as the problem, not the answer. |
| Notion | medium | low | low | ignore | Docs and guest pages like Notes; part of the stack the thread wants to leave. |
| Loftside | low | low | medium | investigate | Early, but its link-instead-of-account client access and per-item "client sees / private" flag are ideas ForkMesh has no answer to. |
| NEXPMX | low | low | low | ignore | Indian-market agency suite with GST and UPI; "one memory for people and AI" is its pitch. |
| Daily Tracker | low | low | low | ignore | Time and routines only; close to the Timers and Tasks packages. |
| Ignara | low | low | low | ignore | Early, for one person, not an agency. |
| Polynet | low | low | low | ignore | A community platform with an AI agent, not client work. |
| Biscuit | low | low | low | ignore | A business-launch service; the reply's claim to build "exactly this" is not visible on its site. |

## What it means for ForkMesh

- **Not our market.** Every tool here treats the client and the invoice as
  the centre. ForkMesh's centre is the repository; nothing in the thread asks
  for Git, and no tool in it hosts code.
- **What we already have.** Tasks, repository projects and milestones,
  encrypted chat rooms and direct messages, Notes, Calendar, the CRM package,
  the Ledger and Workflows with Stripe and PayPal packs already cover
  project tracking, team chat, docs and contacts for a team whose members
  have accounts.
- **What we do not.** A client who has no account cannot see a project, and
  ForkMesh does not issue invoices or take payments for its users. Both are
  the thread's core asks. Building invoicing would mean handling users'
  money, which `payments-and-plans.md` rules out (Patreon is the only rail).
- **Ideas worth keeping.** Per-item client visibility and a revocable
  read-only link per outside person (Loftside), and an MCP server that lets
  an agency's agents use the workspace as their shared record (Taskip and the
  in-house build in the thread), which ForkMesh's MCP server already does for
  repositories and tasks.

## Not listed

- In-house systems named in replies (Debridger's internal tool, a C# agency
  app, Bino Agency's app by Scaling.pt, jung.dev's, Elluminati's): not public
  products.
- Tools only in repliers' profiles (Stepkeep, an SOP recorder for Chrome;
  Giginvoicer): not offered in the thread as an answer.
- Offers to build it as a service (codeair.tech and others): not products.
