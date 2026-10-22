"""URL route patterns for the ForkMesh relay Worker.

This module holds every compiled path regex the Worker's router (`Default._route`
in ``entry.py``) matches requests against, plus the git smart-HTTP endpoint
patterns. Keeping the route table in one small module makes the request surface
easy to scan without paging through the 11k-line worker, and lets the Worker and
tests import a single source of truth for "which path shape maps to which
handler".

Only pure ``re``-compiled patterns live here — no runtime state, no js/workers
imports — so this module loads standalone (the test suite parses it the same way
it parses ``entry.py``).
"""

import re

# Each room exposes a WebSocket (/ws) and a read-only live client count
# (/clients); the Durable Object picks behavior from the upgrade header.
REPO_ROOM_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/rooms/([^/]+)/(?:ws|clients)$")
# User-created public/private chat channels. Public paths use only
# opaque 128-bit identifiers; human-readable channel names stay encrypted.
CHAT_CHANNELS_RE = re.compile(r"^/api/chat/channels/?$")
CHAT_CHANNEL_RE = re.compile(r"^/api/chat/channels/([0-9a-f]{32})/?$")
CHAT_CHANNEL_MEMBERS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/members/?$")
CHAT_CHANNEL_ROOM_ACCESS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/room-access/?$")
# Retained (still-encrypted) backlog of one channel room. Desktop clients hold
# no WebSocket into these rooms, so they replay the World office's channel chat
# over this read-only poll instead.
CHAT_CHANNEL_HISTORY_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/history/?$")
CHAT_CHANNEL_PENDING_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/pending/?$")
CHAT_CHANNEL_WEBHOOKS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/webhooks/?$")
CHAT_CHANNEL_WEBHOOK_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/webhooks/([0-9a-f]{32})/?$")
CHAT_WEBHOOK_DELIVERY_RE = re.compile(
    r"^/api/chat/webhooks(?:/[0-9a-f]{64})?/?$")
CHAT_CHANNEL_INVITE_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/invite/?$")
CHAT_INVITE_RE = re.compile(
    r"^/api/chat/invites/(?:[0-9a-f]{64}|preview|accept)/?$")
CHAT_CHANNEL_WS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/ws/?$")
# One-to-one direct messages use opaque conversation identifiers and a
# participant-only authorization policy with no administrator bypass.
CHAT_DIRECT_MESSAGES_RE = re.compile(r"^/api/chat/direct-messages/?$")
CHAT_DIRECT_MESSAGE_USERS_RE = re.compile(
    r"^/api/chat/direct-messages/users/?$")
CHAT_DIRECT_MESSAGE_ROOM_ACCESS_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/room-access/?$")
CHAT_DIRECT_MESSAGE_READ_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/read/?$")
CHAT_DIRECT_MESSAGE_PENDING_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/pending/?$")
CHAT_DIRECT_MESSAGE_DELIVERED_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/delivered/?$")
CHAT_DIRECT_MESSAGE_WS_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/ws/?$")
CHAT_READS_RE = re.compile(r"^/api/chat/reads/?$")
CHAT_BLOCKS_RE = re.compile(r"^/api/chat/blocks/?$")
# Issue inbox: signed submissions from people without write access to the repo.
REPO_ISSUES_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/issues$")
# Pull-request inbox: signed PR submissions from any node.
REPO_PULLS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/pulls$")
# Authenticated, asynchronous merge of one exact open pull request.  The
# repository alias rewrite runs before this route, so the handler always
# authorizes and dispatches against the canonical backing-node namespace.
REPO_PULL_MERGE_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/pulls/([1-9][0-9]{0,8})/merge$")
# Authenticated, asynchronous merge of the base branch into one exact open pull
# request's head branch ("Update branch"). Same exact-OID, idempotent, audited
# contract as the merge above; it moves only the pull request's own branch.
REPO_PULL_UPDATE_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/pulls/([1-9][0-9]{0,8})/update$")
REPO_PULL_RULES_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/pulls/([1-9][0-9]{0,8})/rules$")
REPO_PULL_ATTENTION_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/pulls/([1-9][0-9]{0,8})/attention$")
REPO_PULL_CHECKS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/pulls/([1-9][0-9]{0,8})/checks$")
# Branch management typed on the website's Branches page (adhoc #2098). The
# relay runs no Git: POST queues one delete/update/create for the repository's
# own node, GET reads the queue back, and DELETE?id= cancels one that has not
# been taken yet.
REPO_BRANCH_OPS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/branches/ops$")
REPO_BRANCH_OP_RESULTS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/branches/ops/results$")
REPO_BRANCH_DELETE_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/branches/delete$")
# Owner/write-authorized, bounded redacted run summaries from an attested
# mirror Actions executor. No workflow variables or public catalog data use
# this endpoint.
REPO_ACTION_RUNS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/actions/runs$")
# Owner/write-authorized control request couriered to the one mirror Actions
# executor the browser names (www/docs/operations/mirror-actions-control.md).
REPO_ACTION_CONTROL_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/actions/control$")
REPO_ACTION_RUN_LOG_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/actions/runs/([0-9]{1,16})/log$")
REPO_ACTION_SECRETS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/actions/secrets(?:/([^/]+))?$")
ORG_ACTION_SECRETS_RE = re.compile(
    r"^/api/org/([^/]+)/actions/secrets(?:/([^/]+))?$")
# Commit-comment inbox: signed per-commit comments from any node.
# Discussion inbox: signed discussion open/comment submissions from any node.
REPO_DISCUSSIONS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/discussions$")
# Content-free tallies of inbox items no online node (source of truth or an
# approved mirror) has merged yet, so the website can badge tabs with
# "N pending" without owner auth (counts only — the items themselves stay
# encrypted and owner/mirror-gated). Any online mirror drains the queue by
# merging submissions straight into the branch it serves.
REPO_PENDING_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/pending$")
# Thread subscriptions (issue #361): a node signs a subscribe/unsubscribe for one
# issue or PR so it gets notified of every reply, not just mentions of it.
REPO_SUBSCRIBE_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/subscribe$")
# Frozen issue-bounty compatibility route. New wallet/create/payout actions fail
# closed; status reads expose migration-only historical metadata without keys.
REPO_BOUNTY_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/bounty$")
# Private-repo collaborator ACL (issue #9): owner-signed grant/revoke/list of the
# accounts a private repo is shared with.
REPO_SHARES_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/shares$")
# Authenticated daily security-scan ingest and visibility-gated read models.
# Public readers receive the compact redacted clipboard only; rich history is
# owner-authorized and private repositories otherwise remain indistinguishable
# from missing repositories.
REPO_SECURITY_SCANS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/security-scans/"
    r"(lease|ingest|latest|history|triage)$")
# Public mirror health for a logical repo group.
REPO_MIRRORS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/mirrors$")
REPO_TRAFFIC_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/traffic$")
# Exact-node verification. GET preserves the existing control-plane README
# reachability probe; POST checks the registered public Smart HTTP URL and
# records its repository-scoped signed-ref verdict without failing over.
REPO_MIRROR_REACHABILITY_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/mirrors/([^/]+)/reachability$")
# Catalog-facing About details editable from the dashboard by the source owner.
REPO_ABOUT_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/about$")
REPO_LOGO_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/logo$")
REPO_LOGO_SUGGESTIONS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/logo-suggestions$")
# Agent-session sync (adhoc #182): desktop node push/drain of Claude Code agent
# sessions for a repo (signed the same way as issue-inbox drain), the
# owner-authorized retrieval of that same ciphertext list, and owner-sealed
# prompts consumed only by the local recipient key.
REPO_AGENTS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/agents$")
REPO_AGENTS_LIST_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/agents/list$")
REPO_AGENTS_ACK_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/agents/ack$")
REPO_DESKTOP_COMMANDS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/desktop-commands$")
REPO_DESKTOP_COMMANDS_ACK_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/desktop-commands/ack$")
REPO_AGENTS_PROMPT_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agents/([^/]+)/prompt$")
# One agent session's owner-sealed snapshot (including its bounded transcript
# tail). An authorized client can retrieve the ciphertext, but only the owner
# device holding the hybrid private key can decrypt it.
REPO_AGENTS_TRANSCRIPT_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agents/([^/]+)/transcript$")
# "Move to cloud" on the desktop's agent detail pane: the node that has been
# running a session hands its continuation to the owner job queue, signed with
# its own owner key (a desktop holds no browser session).  The work itself
# travels in Git on the session's published branch; only the instruction to
# resume it travels here.
REPO_AGENT_HANDOFF_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agents/([^/]+)/handoff$")
# Owner-account web agent queue.  Unlike the owner-sealed snapshot transport
# above, this is an explicit browser-to-owned-device command surface: the
# signed-in owner chooses one of their agent-capable desktop nodes, and that
# node still claims the job and runs the local fail-closed safety gate.
REPO_OWNER_AGENT_SESSIONS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agent-sessions$")
REPO_OWNER_AGENT_SESSION_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agent-sessions/([^/]+)$")
REPO_OWNER_AGENT_SESSION_ATTACHMENT_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agent-sessions/([^/]+)"
    r"/attachments/([0-9a-f]{32})$")
# The same owner queue read across every repository the account owns, so a
# desktop or a phone can see that a mirror is running something without asking
# one repository at a time (no client polls; see
# www/docs/operations/polling-elimination.md).
ACCOUNT_AGENT_SESSIONS_RE = re.compile(r"^/api/account/agent-sessions$")
REPO_OWNER_AGENT_SESSION_STREAM_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agent-sessions/([^/]+)/(?:stream|live)$")
ORG_AGENT_BOT_STREAM_RE = re.compile(
    r"^/api/orgs/([^/]+)/repos/([^/]+)/agent-bots/([^/]+)/(?:stream|live)$")
REPO_ORG_AGENT_JOB_STREAM_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/org-agent-jobs/([1-9][0-9]{0,18})/stream$")
ACCOUNT_DESKTOP_AGENTS_RE = re.compile(r"^/api/account/desktop-agents$")
# The opposite direction to REPO_AGENT_HANDOFF_RE: a desktop takes a queued web
# session over and runs it itself ("Continue on this desktop").  Signed by the
# claiming node the same way the job queue below is, never by a browser.
REPO_OWNER_AGENT_SESSION_CLAIM_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agent-sessions/([^/]+)/claim$")
REPO_OWNER_AGENT_SESSION_DIFF_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/agent-sessions/([^/]+)/diff$")
ORG_AGENT_BOT_DIFF_RE = re.compile(
    r"^/api/orgs/([^/]+)/repos/([^/]+)/agent-bots/([^/]+)/diff$")
# Organization-member coding bots. The organization routes retain the public
# org alias for membership checks; mirror jobs use the selected node namespace
# and owner signatures.
ORG_AGENT_BOTS_RE = re.compile(
    r"^/api/orgs/([^/]+)/repos/([^/]+)/agent-bots$")
ORG_AGENT_BOT_RE = re.compile(
    r"^/api/orgs/([^/]+)/repos/([^/]+)/agent-bots/([^/]+)$")
ORG_AGENT_BOT_ATTACHMENT_RE = re.compile(
    r"^/api/orgs/([^/]+)/repos/([^/]+)/agent-bots/([^/]+)"
    r"/attachments/([0-9a-f]{32})$")
REPO_ORG_AGENT_JOBS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/org-agent-jobs$")
REPO_ORG_AGENT_JOB_RESULT_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/org-agent-jobs/([1-9][0-9]{0,18})/result$")
# One image attached to a leased job; the node's signed query plus the lease.
REPO_ORG_AGENT_JOB_ATTACHMENT_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/org-agent-jobs/([1-9][0-9]{0,18})"
    r"/attachments/([0-9a-f]{32})$")
# Owner-only encryption policy and recipient-key registration for private
# repository/agent data. The relay stores public bundles and opaque envelopes,
# never recipient private keys.
REPO_PRIVACY_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/privacy$")
# Authorized download of one owner-sealed private mirror archive. Client-facing
# private transport is deliberately identity-free: platform request logs see
# only a random 256-bit ciphertext locator, never an owner or repository name.
# Authorization is carried in the non-forwarded HTTP Authorization header.
PRIVATE_REPLICA_ACCESS_RE = re.compile(
    r"^/api/private-replicas/([0-9a-f]{64})$")
# The `host` action stays parseable only so the retired repository tunnel can
# be recognized and answered with one cacheable 410 (an Upgrade to it is
# already rejected at the edge with 426). The other actions are live: Default
# routes those authorized reads through direct HTTPS.
REPO_HOST_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/(host|tree|blobs|blob|raw|history|commit|compare|merge-preview|branches|search|stats|sizes|pulse|issue-index|issue-search)$")
# Stable content-addressed release URL. Default streams it from an attested
# direct-HTTPS endpoint; the host control socket rejects this path.
RELEASE_BLOB_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/releases/blob/sha256/([0-9a-f]{64})$")
# Per-artifact download counts for a repo's releases (issue: Releases tab).
REPO_RELEASE_DOWNLOADS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/releases/downloads$")
# Git smart-HTTP clone endpoints: git clone https://host/<node>/<repo>. The
# repository group never includes a trailing ".git", so /<node>/<repo>.git (the
# spelling Go module paths, Cargo, and the published ssh:// URL use) routes to
# the same repository (issue #495, www/docs/dependency-workflows.md).
GIT_INFO_RE = re.compile(r"^/([^/]+)/([^/]+?)(?:\.git)?/info/refs$")
GIT_PACK_RE = re.compile(r"^/([^/]+)/([^/]+?)(?:\.git)?/git-upload-pack$")
# Reserved receive-pack path. It fails closed until a direct-HTTPS write
# protocol is available and never falls back to the control socket.
GIT_RECEIVE_RE = re.compile(r"^/([^/]+)/([^/]+?)(?:\.git)?/git-receive-pack$")
# --- Organizations + teams (issue #388) ---------------------------------------
# Orgs are user-created namespaces that serve linked repos at /<org>/<repo>
# instead of the hosting node's name. POST /api/orgs creates one (session
# auth); the per-org members/teams/repos collections manage the roster, the
# permission teams, and the repo alias map.
ORGS_RE = re.compile(r"^/api/orgs$")
ORG_RE = re.compile(r"^/api/orgs/([^/]+)$")
ORG_MEMBERS_RE = re.compile(r"^/api/orgs/([^/]+)/members$")
ORG_TEAMS_RE = re.compile(r"^/api/orgs/([^/]+)/teams$")
ORG_TEAM_MEMBERS_RE = re.compile(r"^/api/orgs/([^/]+)/teams/([^/]+)/members$")
ORG_REPOS_RE = re.compile(r"^/api/orgs/([^/]+)/repos$")
ORG_BOT_TOKENS_RE = re.compile(r"^/api/orgs/([^/]+)/bot-tokens$")
# The Discord connector is organization-only. Generic ORG_RE must not swallow
# configuration, selected-channel messages, or its one-time OAuth start action
# before membership/owner checks run. The callback itself is global and fixed
# so one exact Discord OAuth redirect URI works for every organization.
ORG_DISCORD_RE = re.compile(
    r"^/api/orgs/([^/]+)/discord(?:/(messages|oauth/start))?$")
DISCORD_OAUTH_CALLBACK_RE = re.compile(
    r"^/api/integrations/discord/callback$")
MAILTRAP_WEBHOOK_RE = re.compile(
    r"^/api/integrations/mailtrap/webhook/?$")
MAIL_TRACKING_RE = re.compile(
    r"^/api/mail/t/(o|c|u|l)/((?:[0-9a-f]{32}|[0-9a-f-]{36})\.[0-9a-f]{24})"
    r"(?:\.gif|\.png)?$")
MAIL_FOOTER_IMAGE_RE = re.compile(
    r"^/api/mail/t/f/([0-9a-f]{32}\.[0-9a-f]{24})\.png$")
MAIL_FOOTER_RE = re.compile(r"^/api/mail/footer/?$")
# Self-serve plan surface (issue #488): per-account usage meters and the
# one-per-account trial register. Session-authenticated; provider-free.
PLAN_SERVICES_RE = re.compile(r"^/api/plans/(usage|trial)/?$")
# ForkMesh Ads (app/src/ad_network.py): member-written, human-reviewed text
# ads. One family so the module owns its own sub-routing and 404s.
AD_NETWORK_RE = re.compile(r"^/api/ads(?:/[A-Za-z0-9/_-]*)?$")
PATREON_OAUTH_CALLBACK_RE = re.compile(
    r"^/api/integrations/patreon/callback/?$")
PATREON_WEBHOOK_RE = re.compile(
    r"^/api/integrations/patreon/webhook/?$")
BOT_SESSION_RE = re.compile(r"^/api/bot/session$")
# Organization-only, non-custodial succession. The optional action is parsed by
# the isolated API module; the general ORG_RE cannot swallow this subresource.
ORG_SUCCESSION_RE = re.compile(
    r"^/api/orgs/([^/]+)/succession(?:/([^/]+))?$")
# Achievement badges: GET /api/badges is the public fixed catalog; GET on the
# per-account resource lists what an account has earned (public), while
# POST/DELETE grant or revoke a badge (platform-administrator session only).
BADGES_RE = re.compile(r"^/api/badges$")
BADGE_ACCOUNT_RE = re.compile(r"^/api/badges/([^/]+)$")
# Organization-admin digest controls and previews for the org's linked public
# repositories. These controls can suppress org-alias digests, but never
# override the backing repository owner's federation switch.
ORG_FEDIVERSE_RE = re.compile(r"^/api/orgs/([^/]+)/fediverse$")
ORG_PULL_RULES_RE = re.compile(r"^/api/orgs/([^/]+)/pull-rules$")
# Personal + organization calendar collection, event, and RSVP resources.
CALENDAR_RE = re.compile(
    r"^/api/calendar(?:/([0-9a-f]{32})(?:/(rsvp))?)?/?$")
# Repo-scoped API prefix, matched once by the org-alias rewrite so an org's
# /api/repo/<org>/<repo>/... URLs are re-routed to the linked node's repo
# before any of the per-endpoint patterns above run.
REPO_API_PREFIX_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$")
# Account API: reserve/finalize/login and GET /api/accounts/{name} are all
# single-segment, so this one pattern gates the whole accounts_handler dispatch.
ACCOUNTS_RE = re.compile(r"^/api/accounts/([^/]+)$")
# Public native contribution read model for one profile.
ACCOUNT_CONTRIBUTIONS_RE = re.compile(
    r"^/api/accounts/([^/]+)/contributions$"
)
# Follow/unfollow a public profile: /api/accounts/{name}/follow
ACCOUNT_FOLLOW_RE = re.compile(r"^/api/accounts/([^/]+)/follow$")
ACCOUNT_AVATAR_RE = re.compile(r"^/api/accounts/([^/]+)/avatar$")
# Paid microjobs: GET /api/microjobs is the public job catalog (plus the
# caller's own claims when a session is supplied) and POST
# /api/microjobs/claims submits one verified claim. Both live behind one
# pattern so the isolated microjobs module owns its whole subtree.
MICROJOBS_RE = re.compile(r"^/api/microjobs(?:/claims)?/?$")
AFFILIATES_RE = re.compile(
    r"^/api/(?:admin/)?affiliates"
    r"(?:/(?:join|leads|leads/[a-f0-9-]{8,64}/activity))?/?$")
PLUGIN_RACE_RE = re.compile(
    r"^/api/(?:admin/)?plugin-race(?:/(?:entrant|plugins))?/?$")
# Hackathon API subtree: the campaign, join, the participant's own view,
# share presses, the public boards, squad pages and the unfurl cards all
# live behind one pattern so the isolated runtime module owns the whole
# namespace and entry.py dispatches it once.
HACKATHON_API_RE = re.compile(r"^/api/hackathon(?:/.*)?$")
CHALLENGES_API_RE = re.compile(r"^/api/challenges(?:/.*)?$")
WORKFLOWS_API_RE = re.compile(r"^/api/workflows(?:/.*)?$")
# Personalised invitation page: /hackathon/join/{name}. Server-rendered
# because it carries the inviter's live standing, and proxied by the
# marketing Worker so the link a participant shares stays on the apex.
HACKATHON_INVITE_RE = re.compile(r"^/hackathon/join/([^/]+)/?$")
# One squad's public page: /hackathon/squad/{code}.
HACKATHON_SQUAD_RE = re.compile(r"^/hackathon/squad/([^/]+)/?$")
# A challenge link: /hackathon/challenge/{token}. Rendered here for its live
# accepted and beaten counts, proxied by the marketing Worker like the rest.
# An investor deck link minted by the contacts package for one person:
# /pitch-deck/{token}. The App renders it (the document is stamped with the
# reader's name and reports page views under the same token); the marketing
# Worker proxies it so the link is spelled on the apex like every share link.
PITCH_DECK_PAGE_RE = re.compile(r"^/pitch-deck/([a-z0-9]{10,40})/?$")
PITCH_DECK_EVENTS_RE = re.compile(
    r"^/api/pitch-deck/([a-z0-9]{10,40})/events$")
HACKATHON_CHALLENGE_RE = re.compile(
    r"^/hackathon/challenge/([A-Za-z0-9_-]{16,40})/?$")
# One valid project's public page: /hackathon/projects/{submission id}. The
# gallery itself, /hackathon/projects, is a static marketing document.
HACKATHON_PROJECT_RE = re.compile(r"^/hackathon/projects/([0-9a-f]{32})/?$")
# Referral short link: /r/{name} counts a click and bounces to /signup.
REFERRAL_LINK_RE = re.compile(r"^/r/([^/]+)$")
# Rendered social-preview card for one share link: the og:image the /r/{name}
# preview page points at, so a Mastodon/Slack unfurl shows that referrer's
# live click + signup counters.
REFERRAL_CARD_RE = re.compile(r"^/api/referrals/([^/]+)/card\.png$")
# --- ActivityPub federation ---------------------------------------------------
# User actor document + its inbox/outbox/followers/following collections.
AP_USER_RE = re.compile(r"^/ap/users/([^/]+)$")
AP_USER_SUB_RE = re.compile(
    r"^/ap/users/([^/]+)/(inbox|outbox|followers|following)$")
# Repository actor (followed as @owner.repo@<domain> from the fediverse).
AP_REPO_RE = re.compile(r"^/ap/repos/([^/]+)/([^/]+)$")
AP_REPO_SUB_RE = re.compile(
    r"^/ap/repos/([^/]+)/([^/]+)/(inbox|outbox|followers|following)$")
# Local ActivityPub object (a published Note), 32-hex uuid.
AP_OBJECT_RE = re.compile(r"^/ap/o/([0-9a-f]{32})$")
# An image embedded in that Note's body, re-served from its stored base64 so
# remote servers (which cannot fetch data: URLs) have a real URL to attach.
AP_OBJECT_MEDIA_RE = re.compile(r"^/ap/o/([0-9a-f]{32})/media/([0-9]+)$")
# Remote fediverse replies attached to a repo thread, readable by clients.
REPO_FEDI_COMMENTS_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/fedi-comments$")
# Owner-node push of canonical repo announcements (releases, merged PRs) into
# the fediverse — events the relay never observes through the signed inboxes.
REPO_AP_PUBLISH_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/ap-publish$")
# Repo-owner digest preview. Automatic repository events are accumulated in an
# encrypted, bounded queue and published at most once per 24 hours; this route
# lets the owner inspect the exact public digest text without publishing it.
REPO_AP_DIGEST_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/fediverse-digest$")
# Repo owner's fediverse-post management surface (dashboard): list the repo
# actor's federated posts and delete one (broadcasts a Delete(Tombstone) so it
# disappears from Mastodon). Session/owner-key authed, never public.
REPO_AP_POSTS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/ap-posts$")
# Repo-owner webhook management (dashboard Settings tab): register a
# third-party endpoint that receives signed forkmesh-webhook-v1 deliveries when
# the repository is pushed, a pull request is merged, or a release is
# published. POST-only so the session token never enters a query string.
REPO_WEBHOOKS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/webhooks$")
REPO_PROVIDER_MIRROR_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/provider-mirror$")
REPO_ARCHIVE_SYNC_RE = re.compile(
    r"^/api/repo/([^/]+)/([^/]+)/archive-sync$")
# Owner-uploaded repo branding (logo/banner PNG) served publicly — referenced
# by the repo's fediverse actor document as its avatar/header.
REPO_MEDIA_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/media/(logo|banner)\.png$")
# Rendered social-preview info card (repo stats grid): the repo page's
# og:image, so Mastodon/Slack/Twitter unfurls show the repo's details.
REPO_CARD_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/card\.png$")
# Star/unstar a repo: GET returns the public count (+ the caller's own starred
# state when a session is supplied); POST/DELETE toggle it for the logged-in
# account (session-token authenticated, same as ACCOUNT_FOLLOW_RE).
REPO_STAR_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/star$")
REPO_REACTIONS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/reactions$")
REPO_TIPS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/tips$")
CUSTOM_REACTIONS_RE = re.compile(r"^/api/reactions/custom(?:/(manage))?$")
CUSTOM_REACTION_ASSET_RE = re.compile(
    r"^/api/reactions/custom/asset/([0-9a-f]{40})\.(svg|png|gif|webp)$")
COMMENT_GIFS_RE = re.compile(r"^/api/gifs/(config|search|trending|settings)$")
COMMENT_TIPS_RE = re.compile(r"^/api/tips/(config|recipients|verify|settings)$")
REPO_VIEW_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/view$")
REPO_VIEWS_RE = re.compile(r"^/api/repo-views/?$")
# Forks for this repo: GET returns the public list of repositories that publish
# this one as their `forkedFrom`; POST durably asks one of the caller's linked
# managed mirrors to create their fork. The public edge still appears only
# after that mirror publishes its own signed catalog record.
REPO_FORKS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/forks$")
REPO_REMOTE_FORKS_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/remote-forks$")
REPO_PULL_BATCH_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/pull-batch$")
