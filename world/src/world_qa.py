"""On-demand World QA desk deck and review board.

Cloudflare validates a Python Worker by compiling and executing its
entrypoint under the isolate memory limit.  The QA deck's card catalog and
review handler are only needed by the World QA route, so entry.py loads this
module on the first request that reaches it instead of spending scarce
Pyodide startup memory on every isolate.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the QA board implementation."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


WORLD_QA_DECK_REVISION = "2026-10-24-world-voice-camera-channels-32"
# The physical desk paints only five cards per page, but its catalog must
# include every bounded source: built-ins, dynamically routed QA items, and
# the organization's encrypted QA-ready tasks. Organization tasks are capped
# at 2,000 and the routed board is independently bounded, so 4,096 is a hard
# response ceiling rather than an arbitrary visible-card truncation.
WORLD_QA_MAX_CARDS = 4096
WORLD_QA_CARDS = (
    ("world-voice-camera-channels",
     "Voice, camera avatars, bubbles and the channel list",
     "Sign in on two browsers and stand near each other. Turn the microphone "
     "on in one and confirm both join #general voice, hear each other, and see "
     "a word badge on the speaker's right shoulder that counts up and blinks "
     "green while talking. Turn the camera on and confirm the avatar's head, "
     "face and raised hand follow you, and a thumbs up sends a reaction to the "
     "speaker. Select the other avatar, start a privacy bubble, join it on the "
     "other browser, and confirm the bubble shows and nobody outside hears it. "
     "Open the # list and confirm World channels, voice rooms, Office rooms, "
     "channels and messages open in the World. Click the performance badge and "
     "send a diagnostic report."),
    ("world-debug-live-telemetry-strip",
     "DEBUG live telemetry strip",
     "Open the World and hover DEBUG. Confirm the strip charts the nine health "
     "dots in the orb's own order, then TRIS, CPU, P95, JANK, ANIM, and AVTR, "
     "with MEM across the bottom. DRAW must read draw calls per frame and TRIS "
     "the triangle count, each with its own moving trace. Walk into a busy "
     "district and confirm every trace updates once a second and turns yellow "
     "or red on its own threshold. Hide the tab, return, and confirm the "
     "renderer traces gap for the paused seconds instead of dipping to zero."),
    ("world-compact-debug-chat-orbs",
     "Compact debug and unified activity orbs",
     "Open the World on desktop and mobile. Confirm DEBUG and CHAT are "
     "logo-sized circles while closed. DEBUG must show nine green, yellow, "
     "or red metric dots and reveal its complete panel on hover or keyboard "
     "focus. CHAT must show the latest speaker avatar plus unread count and "
     "open a translucent composer with channel selection, image attachment, "
     "separate chat and task buttons, and a second task-routing step for "
     "human or agent plus team. Send chat and task updates, then confirm every "
     "chat or status notice flows above the composer and fades after ten "
     "seconds without removing the underlying chat history."),
    ("world-repository-agent-live-terminals",
     "Interactive repository agent terminals",
     "As an Engineering team member, open a repository with a running Claude "
     "or Codex session. Confirm its small robot screen shows the exact task, "
     "status, mirror, and latest terminal line. Click it and confirm the exact "
     "session opens with transcript plus a working secure follow-up prompt. "
     "Repeat as a non-engineer and confirm the controls remain inaccessible."),
    ("world-repository-record-view-pads",
     "Repository list order, footer, and viewing pads",
     "Open forkmesh/forkmesh in the World. Confirm the PR and Issue boards "
     "each grow only as tall as the records on the current page, the newest "
     "record is at the bottom, and the large open count plus pagination sit "
     "below the list. Click the floor pad beneath each board and confirm the "
     "World enters first-person at a distance and pitch that frame the complete "
     "panel; walk away and confirm ordinary first-person controls still work."),
    ("world-continuous-city-round", "Continuous World city foundation",
     "Walk between the central nodes, repositories, users, billboards, and "
     "Office without jumping a gap. Confirm every route rests on one finished "
     "city foundation with grass and concrete textures and no visible seam."),
    ("world-start-here-progress", "START HERE destination progress",
     "Open a fresh World profile and use the START HERE map. Visit the users "
     "circle, nodes, repository district, Office, and an exploration stop. "
     "Confirm each bounded step checks once and remains checked after reload."),
    ("world-roof-jump-seating", "Roof jump and universal seating",
     "Ride to the Office roof, press Space, clear the perimeter, and confirm "
     "the avatar returns outdoors with an attendance checkout. Click several "
     "park benches and chairs and confirm each seats and releases the avatar."),
    ("world-connected-beach-car", "Connected beach and driveable car",
     "Follow the eastern road to the beach, drive the car both directions, "
     "then dismount. Confirm sand, water, the local horizon, and seating load "
     "without a remote request, invisible wall, or disconnected land."),
    ("world-perimeter-bike-loop", "Perimeter bicycle route",
     "Ride both bicycles around the complete outer loop. Confirm the lane "
     "stays outside activity areas, remains on solid land, and reconnects "
     "without a narrow bridge or collision stop."),
    ("world-billboard-circle", "Circular public billboard layout",
     "Visit the billboard district from the center and perimeter. Confirm all "
     "public boards form one readable tangent circle, only one leaderboard "
     "panel exists, and no obsolete center monument remains."),
    ("world-github-dark-system", "Shared GitHub-dark World controls",
     "Open the map, settings, account, repository detail, Office task, and "
     "notice panels. Confirm they share the same dark surfaces, borders, "
     "button radii, focus states, and readable contrast."),
    ("world-local-daylight-textures", "Local daylight and optimized textures",
     "Compare the World at daytime and nighttime or simulate the local clock. "
     "Confirm sky and sun follow local time, chest time is local, organization "
     "stars render, and grass, path, and beach textures load from local WebP."),
    ("world-node-delete-alias-regression", "Canonical World node deletion",
     "As is_admin, delete a disposable cabinet whose visible machine name, "
     "account, and node ID differ. Confirm the API resolves the canonical node, "
     "removes only that cabinet and owner link, and plays the removal effect."),
    ("world-admin-member-detail-email-state",
     "Admin member detail and email state",
     "Open a registered member from their World avatar. Confirm the side "
     "panel always shows either a green VERIFIED EMAIL check or a red EMAIL "
     "NOT VERIFIED mark. As is_admin, select Open admin user detail and "
     "confirm the secret admin console opens directly to only that user's "
     "record with prefilled password-reset and verification tools. Repeat as "
     "a non-admin and confirm the link is absent."),
    ("world-repository-split-panels-agent-dock",
     "Split repository work panels and agent dock",
     "Open forkmesh/forkmesh in the World. Confirm pull requests occupy the "
     "left panel and issues the right panel, each uses one 25-record column, "
     "shows a star-sized OPEN count, and only bottom controls contain Prev, "
     "Next, page, and range. Page each side independently and open one card "
     "from each. Confirm the "
     "repository name is part of the angled commit-activity pedestal. While "
     "an Engineering-authorized Claude or Codex session runs for this repo, "
     "confirm one compact 45-degree robot screen appears in front with its "
     "provider, live state, task snapshot, and target node; confirm a "
     "non-Engineering member sees no agent terminal."),
    ("world-member-click-detail", "Clickable World member information",
     "Click the body or chest of your own avatar and two other visible users, "
     "including one inside the Office. Confirm a subtle blue outline follows "
     "the selected avatar and the side panel shows the privacy-filtered member, "
     "status, verified-email state, public teams, activity, nodes, and "
     "Fediverse information. Confirm private IP and full User-Agent never "
     "appear in this general member panel."),
    ("world-admin-node-delete-machine-name", "Admin World node deletion",
     "As is_admin, open a mirror cabinet whose operator account differs from "
     "its machine name. Confirm the danger zone asks for DELETE plus the mirror "
     "machine name, rejects a mismatched confirmation, and deletes only after "
     "the browser confirmation. Confirm the cabinet list force-refreshes and "
     "the removed mirror disappears; verify a non-admin sees no delete form."),
    ("world-annotated-cardinal-districts", "Cardinal World district layout",
     "From an overhead view confirm exactly four broad paved routes leave the "
     "central live-node plaza. Walk each route and confirm its rounded join is "
     "seamless: east reaches every fully expanded repository, west reaches all "
     "bulletins and boards, south reaches the member/campfire circle, and north "
     "reaches the Office. Confirm no mirror cabinet moved out of the center and "
     "that old saved board coordinates do not pull a board back into town."),
    ("world-flagship-always-expanded", "Flagship repository opens without mirror delay",
     "Open a fresh World tab with normal network throttling. Confirm the "
     "forkmesh/forkmesh repository wheel expands as soon as its tree arrives "
     "and never collapses to a flat MIRRORS SYNCING disc while mirror status "
     "metadata converges. Confirm files remain clickable after the mirror "
     "details finish loading."),
    ("world-avatar-identity-refresh", "Immediate avatar flags and verified pin",
     "Join from a newly verified account with country sharing enabled. From a "
     "second World tab, confirm its country flag appears without reloading and "
     "a small green verified check pin is visible on the avatar front. Disable "
     "country sharing and confirm the country is removed without exposing an "
     "email address."),
    ("world-unique-uploaded-faces", "Unique generated and uploaded avatar faces",
     "Compare several users who have no account photo and confirm each gets a "
     "stable, visibly different generated face after reload. In World settings "
     "upload a JPEG, WebP, or PNG and confirm it is center-cropped, reported as "
     "at most 128px/64 KiB, replaces the generated face, and appears in the "
     "top-right account button at the same size as neighboring controls."),
    ("world-cabinet-side-layout", "Cabinet operational and agent side layout",
     "Walk around a live mirror cabinet. Confirm Actions runs face the "
     "operational walkway, server information is on the opposite face, Claude "
     "tasks appear only on the left side, and Codex tasks only on the right. "
     "Open each panel and confirm its existing authorized detail action works."),
    ("world-connected-mixed-landscape", "Connected city and woodland landscape",
     "Walk from Town Square to the repository district and Office. Confirm "
     "both routes are broad continuous pieces of land with no narrow bridge "
     "or invisible gap. Look across the Town Square for warm stone plazas, "
     "edged paths, varied grass pockets, and trees instead of one flat green "
     "surface. Pull the camera below an island edge and confirm finished dirt "
     "layers and hanging roots are visible."),
    ("world-aquarium-control-panel", "Aquarium control panel",
     "Walk to the Office lobby aquarium and confirm a physical reef "
     "controller sits on the tank's front base at its lower-right corner with "
     "nothing floating in the air. Hover each control for the pointer cursor "
     "and glow, then click or tap: FEED lights up and the fish swarm the food "
     "at the surface, TAP flicks the school, the SCHOOL dial steps AUTO, "
     "SPECIES, COLOUR and SCATTER, BACKDROP flips opaque/clear, the GLASS "
     "switch picks curious or shy, FOLLOW makes fish chase the cursor over "
     "the glass, and the LIGHT lever throws the tank light off and on. Tap "
     "the glass itself and confirm nearby fish swim to (or away from) the "
     "spot. The screen reads the live school mode and feeding countdown, and "
     "the light, backdrop and glass preferences survive a reload."),
    ("world-aquarium-user-school", "Users represented by aquarium fish",
     "Compare the public account directory with the Office aquarium. Confirm "
     "there is one small, unlabeled fish per public user, each user's color "
     "and size remain stable after reload, active/recent users swim in upper "
     "lanes, inactive users stay near the bottom, and the full school fits "
     "inside the tank without oversized fish. Wait for school mode and confirm "
     "matching public country, browser, then coarse browser/OS agent groups "
     "briefly form smooth pods before returning to their individual routes."),
    ("world-admin-direct-layout", "Admin direct World layout editing",
     "Sign in as a platform administrator. Drag the visible base of several "
     "Town Square objects and confirm each follows without a tiny edit handle. "
     "Right-drag and Shift-drag left/right to rotate, then reload and confirm "
     "both position and heading persist. Repeat as a non-admin and confirm "
     "objects retain their normal click behavior and cannot be moved."),
    ("world-mirror-live-cabinets", "Live green World mirror cabinets",
     "Open the repository Mirror nodes view and ForkMesh World together. "
     "Confirm mirror2, mirror3, and mirror6 each have a cabinet and every "
     "reachable machine has a steady green roof beacon. Open mirror2 detail "
     "and confirm clone availability and integrity remain truthful even while "
     "the physical-node beacon reports it alive."),
    ("qt-mirror-latest-commit-age", "Mirror latest-commit age",
     "Open the Qt repository Mirror nodes table. Confirm each reported Latest "
     "commit shows its short hash, branch, and an x-ago age derived from that "
     "commit's timestamp; hover it and confirm the full commit evidence remains."),
    ("open-pr-audit", "Open pull-request disposition audit",
     "Open the PR board and compare every record marked Open with "
     "www/docs/operations/open-pull-audit-2026-07-28.md. Confirm PR #52's change "
     "is present on main and its chat disclosure survives a dashboard asset "
     "rebuild. Confirm deleted-head PRs show their recorded stale, represented, "
     "or recover-before-review disposition rather than implying mergeability."),
    ("mirror-service-counters", "Mirror clone and website counters",
     "Open the repository Mirror nodes table and the matching World cabinet. "
     "Confirm mirror6 shows numeric Clones and Websites values, including "
     "truthful zeroes before its first request. Browse one repository page and "
     "complete one clone through an eligible direct mirror, wait for its signed "
     "catalog refresh, and confirm only the corresponding counters increase. "
     "Provision or restart a mirror and confirm both values remain available."),
    ("mirror-actions-cabinet", "Mirror cabinet Actions runs",
     "Sign in as a repository owner or organization writer and walk behind "
     "the active mirror cabinet. Confirm recent Actions runs show running, "
     "done, or error plus a bounded redacted log tail. Click the Actions face "
     "and confirm the full authorized run list opens. Repeat without write "
     "access and confirm no private run or log data appears."),
    ("deploy-lifecycle", "World deployment lifecycle",
     "Start a deployment while the World is open. Confirm the deploy notice "
     "appears immediately, animates while work is active, and ends with a "
     "Refresh button without refreshing the page by itself."),
    ("elevator-camera-lock", "Elevator button camera lock",
     "Enter the Office elevator from two angles. Confirm the camera frames the "
     "buttons during the ride, unlocks on the destination floor, and never "
     "rotates your heading when you step out."),
    ("mobile-movement-stability", "Stable mobile movement",
     "On a signed-in phone, walk continuously for at least two minutes. Confirm "
     "the page never reloads and your position is not reset."),
    ("account-world-settings", "Cross-device World preferences",
     "Change speed or save a view on one signed-in device, then open the World "
     "on another. Confirm the setting and saved view follow the account."),
    ("office-doorway", "Smooth Office doorway",
     "Walk through the Office entrance and back out at normal speed without "
     "twitching. Confirm there is no ledge, invisible stop, delay, gap, zoom "
     "jump, or 180-degree turn."),
    ("sound-toggle", "Master sound button",
     "Start a World audio item, toggle sound off and on, and confirm active "
     "playback actually mutes and restores without starting audio automatically."),
    ("repo-social-orbits", "Repository social avatar orbits",
     "Open the forkmesh/forkmesh repository circle. Confirm Mastodon follower "
     "avatars form the outer orbit and individual Git contributor avatars form "
     "the inner orbit at normal camera angles."),
    ("bulk-import-retirement", "Bulk repository import retirement",
     "Open the repository district and catalog. Confirm the 54 repositories "
     "from the retired mirror2/mirror3 import burst are gone, the temporary "
     "import island is empty, and forkmesh plus unrelated repositories remain."),
    ("repo-pr-board", "Pull-request status and mergeability",
     "Page and scroll through the PR board. Confirm each card shows its actual "
     "state, mergeability score and evidence, and that 25 records fit each page."),
    ("repo-issue-workbench", "In-World issue workbench",
     "Select an issue card. Confirm its full details and comments open in the "
     "sidebar and authorized management actions match the web issue view."),
    ("issue-agent-models", "Issue-to-agent model controls",
     "On an issue card, open both Claude and Codex assignment controls. Confirm "
     "Claude offers Haiku/Sonnet/Opus/Fable and Codex offers Sol/Luna/Terra, "
     "with authorization enforced server-side."),
    ("engineering-agent-access", "Engineering-only agent workspace",
     "As an Engineering member, open Claude and Codex and verify prompt, "
     "re-prompt, transcript and runtime details. Repeat as a non-Engineering "
     "member and confirm neither chat nor controls are visible."),
    ("mirror-agent-installer", "Mirror Claude/Codex installer",
     "In Qt Hosts, run Install Claude + Codex on a reachable mirror. Confirm the "
     "saved managed SSH identity is used, and unreachable versus rejected-key "
     "failures produce different actionable messages."),
    ("mirror-agent-provider-routing", "Signed mirror agent-provider routing",
     "Open Qt Hosts and confirm mirror6 reports both Claude Code and Codex for "
     "the forkmesh-node service account. Start a Claude task from an Engineering "
     "account and confirm it targets mirror6; confirm an offline, stale, or "
     "Claude-missing mirror is never offered or selected."),
    ("headless-agent-checkout", "Headless mirror agent checkout",
     "Provision a new headless mirror and confirm its private service-owned "
     "forkmesh working checkout exists before forkmesh-node starts. Restart the "
     "service twice and confirm the checkout remains selected, the signed "
     "catalog stays fresh, and a Claude or Codex job can be claimed without a "
     "\"no local checkout\" error."),
    ("mirror-agent-status", "Mirror cabinet agent status",
     "Inspect each mirror cabinet side. Confirm agent tasks show running, "
     "stopped, merged, or attention and the authorized detail panel agrees."),
    ("marketing-room-wall", "Marketing task wall and desks",
     "Enter the Marketing floor as a Marketing member. Confirm tasks live on "
     "the wall, only Marketing assignees are offered, each member has a named "
     "desk and attendance calendar, and the old guide/banner are absent."),
    ("marketing-table", "Marketing reclaimed-wood table",
     "Inspect the Marketing room table from above and at seated height. Confirm "
     "it is round reclaimed wood with the supplied chrome ForkMesh cube image "
     "embedded beneath a clear epoxy-like surface. Confirm front-window desks "
     "face outward, chairs face their desks, and names are small desktop plaques."),
    ("marketing-proof", "Private Marketing proof links",
     "As a Marketing member, submit an HTTPS social proof link from your own "
     "desk and confirm it appears there. Verify a non-Marketing account cannot "
     "read or submit any proof records."),
    ("general-chat-board", "Recent #general World board",
     "Open the World beside the event bulletin. Confirm the #general board "
     "fills from retained and live encrypted chat with author, time, message, "
     "image thumbnail/file label and reaction count when present. Click it and "
     "confirm the complete #general chat opens."),
    ("avatar-team-badges", "Avatar team badges",
     "View members from the front-left and confirm each authorized team appears "
     "as a readable badge on the member's left arm, without exposing private "
     "organization data to outsiders."),
    ("admin-guest-network", "Admin-only guest network detail",
     "Join once as a guest. From a verified is_admin account, confirm the "
     "guest's full IP and user-agent appear on their back and copy exactly. "
     "From a regular account, confirm neither value exists in UI or presence."),
    ("http-referrer-board", "HTTP referrer detail",
     "Open the HTTP referrer board. Confirm domains are grouped, the bottom "
     "line shows the actual latest safe full URL, and selecting it opens only "
     "that validated HTTP(S) destination."),
    ("blog-reach", "Blog reach and sharing details",
     "Inspect each blog card and post. Confirm views, unique views, referrer "
     "stats and network distribution are present when available, with bounded "
     "prefilled Mastodon, X and Reddit share drafts."),
    ("repository-settings", "Repository Settings tab",
     "Open a repository. Confirm federation options moved beneath Settings, "
     "About uses a pencil, and only the owner sees the guarded delete-repository "
     "danger action."),
    ("saved-location-share", "Exact saved and shared views",
     "Save a view, rename it, reopen it, then share a right-click location link. "
     "Confirm position, perspective, Office check-in state and camera heading "
     "restore correctly."),
    ("world-loader", "Live World startup preview",
     "Hard-load the World on desktop and mobile. Confirm the real scene remains "
     "visible behind a small animated progress cover and objects do not shift "
     "noticeably as initial data arrives."),
    ("dashboard-home-data", "Dashboard home repositories and blog",
     "Sign in on Dashboard. Confirm Top repositories includes linked "
     "organization aliases with an organization label, Latest from the blog "
     "shows current posts and artwork, and either ForkMesh logo cleanly reloads "
     "the Dashboard."),
    ("fresh-code-surge", "Fresh Code mirror-push effect",
     "Keep the World open while a mirror publishes a new verified commit. "
     "Confirm the cabinet emits the tall green beam and expanding ground "
     "shockwave once the refreshed signed catalog confirms that commit."),
    ("chest-fediverse-activity", "Unified chest Fediverse and activity card",
     "Open your own and another registered member's chest card. Confirm "
     "Fediverse leaves Loading, the card is slightly larger, no separate "
     "activity dot remains, and the darker node-style border reflects account "
     "activity recency."),
    ("qt-host-live-capabilities", "Qt live host and agent CLI status",
     "Add or reopen a saved Qt Host. Confirm its row keeps checking while SSH "
     "comes online, distinguishes provisioning, unreachable, and rejected-key "
     "states, then shows Claude and Codex as Installed or Not installed."),
    ("alert-management-link", "Alert email management destination",
     "Open Manage this alert from a component or scheduled-job email. Confirm "
     "it opens the secret platform is_admin console, scrolls to Operational "
     "alerts, and focuses the checkbox used to enable or disable those emails. "
     "Confirm a non-admin account cannot open or change it."),
    ("admin-node-delete", "Admin-only permanent node deletion",
     "As a platform is_admin, open a disposable node cabinet and confirm the "
     "danger action requires typing DELETE plus the exact node name. Cancel "
     "once, then delete the disposable node and confirm its cabinet, catalog "
     "repositories, endpoint, and owner fleet entry disappear. Confirm the "
     "control is absent for a non-admin."),
    ("qa-history-routing", "QA history tabs and routing",
     "Use the physical Cards, Pass, Fail, and Unsure tabs. Page the shared task "
     "lists, select a task, then as an authorized maintainer send one into "
     "forkmesh/forkmesh issues."),
    ("qa-history-detail", "QA history full-card review",
     "Open Pass, Fail, or Unsure, select a prior task, and confirm its full "
     "title and test instructions appear. Change its verdict with the large "
     "buttons, then use Back to Cards and confirm shared totals update."),
    ("elevator-front-camera", "Front-facing elevator camera and controls",
     "Enter the elevator and confirm its upper-corner security-camera view "
     "frames the large high-contrast buttons on the right wall while looking "
     "out into the World, not back into the Office. Confirm the old view "
     "returns after arrival or exit."),
    ("marketing-office-hours", "Office-only Marketing attendance",
     "Spend time in the World outside the building, then enter and leave the "
     "Office. Confirm Marketing Office Hours increases only for the interval "
     "between the explicit building entry and exit punches."),
    ("marketing-furniture", "Marketing window desks and seating",
     "Visit Marketing and confirm named desks sit against the rear windows "
     "with chairs, while the raised round sealed-wood table has eight visible "
     "chairs and no green floor showing through its top."),
    ("office-landscaping", "Office exterior landscaping",
     "Walk around and through the Office approach. Confirm flowers, bushes, "
     "and low-poly trees surround the island without blocking the bridge, "
     "doorway, or smooth entry and exit."),
    ("twitter-public-feed", "Twitter/X public board fallback",
     "Open the Twitter/X board. If public @forkmesh posts exist, confirm their "
     "text, age, likes, and reposts render. If X returns no public timeline, "
     "confirm the board explains why and its HUMAN TODO section shows the "
     "read-only X API setup action."),
    ("chest-auth-profile-reload", "Authenticated chest profile reload",
     "Open the World while signed in and wait for the temporary guest identity "
     "to become your account. Confirm the chest changes from Loading to your "
     "actual @account Fediverse details without a page refresh. On a walletless "
     "self card, select Add Wallet and confirm the payout editor opens."),
    ("qt-host-plan-cost", "Qt Host provider and plan provenance",
     "Provision a new Vultr mirror, reopen Hosts, and confirm its status row "
     "retains Vultr, plan type, region, and expected monthly cost after the "
     "install finishes and after restarting Qt."),
    ("marketing-initiatives", "Repository issue Marketing initiatives",
     "As an organization manager, open a numbered web issue and choose Move "
     "to Marketing initiatives. Confirm a current Marketing member sees one "
     "deduplicated card on the private Marketing floor panel and can click it "
     "to reopen the exact issue. Confirm non-Marketing members cannot read "
     "the panel or its encrypted issue details."),
    ("admin-error-analytics", "Admin 24-hour error analytics",
     "As a platform is_admin, open the secret admin console and select Error "
     "logs. Confirm the previous-24-hours chart has 24 accessible hourly bars, "
     "equivalent status/method/path/message rows are grouped with counts, and "
     "the bounded redacted raw log remains available below."),
    ("repository-terms-flags", "Repository Terms moderation flags",
     "As a platform is_admin, apply a Terms flag to a disposable repository "
     "with a private note. Confirm the repository list, detail page, and World "
     "portal show a red flag and public category but never the note. Clear it "
     "and confirm the badges disappear after the catalog refresh."),
)
WORLD_QA_CARD_KEYS = frozenset(item[0] for item in WORLD_QA_CARDS)


async def world_qa_handler(env, request):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"ok": False, "error": "method_not_allowed"},
            status=405,
            extra_headers={"Allow": "GET, POST"},
        )
    data = {}
    if method == "POST":
        if not _request_same_origin(request):
            return json_response({"ok": False, "error": "origin_not_allowed"},
                                 status=403)
        try:
            # A failed private task may include one compact screenshot. The
            # image is validated and encrypted below; keep the whole request
            # bounded well below the general upload ceiling.
            data = await bounded_json_request(request, 640 * 1024)
        except RequestBodyTooLarge:
            return json_response({"ok": False, "error": "payload_too_large"},
                                 status=413)
        except Exception:
            return json_response({"ok": False, "error": "invalid_json"},
                                 status=400)
    dynamic_rows = await d1_all(
        env,
        "SELECT item_key,title,how_to_test,added_at FROM world_qa_items "
        "WHERE active=1 ORDER BY added_at DESC,item_key LIMIT ?",
        WORLD_QA_MAX_CARDS,
    )
    deck_cards = list(WORLD_QA_CARDS)
    deck_keys = set(WORLD_QA_CARD_KEYS)
    for row in dynamic_rows or []:
        key = clean_string(row.get("item_key"), 80)
        title = clean_string(row.get("title"), 160)
        how_to_test = clean_string(row.get("how_to_test"), 720)
        if not key or key in deck_keys or not title or not how_to_test:
            continue
        deck_cards.append((key, title, how_to_test))
        deck_keys.add(key)
    deck_cards = deck_cards[:WORLD_QA_MAX_CARDS]
    deck_keys = {item[0] for item in deck_cards}

    async def global_qa_snapshot():
        global_rows = await d1_all(
            env,
            "SELECT item_key,verdict,COUNT(*) AS count "
            "FROM world_qa_reviews GROUP BY item_key,verdict",
        )
        global_reviews = {}
        for row in global_rows or []:
            key = clean_string(row.get("item_key"), 80)
            verdict = clean_string(row.get("verdict"), 12).lower()
            if (
                key not in deck_keys
                or verdict not in ("pass", "fail", "unsure")
            ):
                continue
            counts = global_reviews.setdefault(
                key, {"pass": 0, "fail": 0, "unsure": 0, "total": 0})
            count = max(0, int(row.get("count") or 0))
            counts[verdict] += count
            counts["total"] += count
        tester_rows = await d1_all(
            env,
            "SELECT COUNT(DISTINCT account_bi) AS testers "
            "FROM world_qa_reviews",
        )
        return global_reviews, {
            "pass": sum(item["pass"] for item in global_reviews.values()),
            "fail": sum(item["fail"] for item in global_reviews.values()),
            "unsure": sum(item["unsure"] for item in global_reviews.values()),
            "reviewed": sum(item["total"] for item in global_reviews.values()),
            "testers": max(
                0, int(((tester_rows or [{}])[0]).get("testers") or 0)),
            "total": len(deck_cards),
        }

    global_reviews, global_stats = await global_qa_snapshot()

    account_bi, record = await _account_session_record(env, request, data)
    if not account_bi or not record:
        return json_response({
            "ok": True,
            "authenticated": False,
            "authorized": False,
            "requiredTeam": "",
            "requiresAuthentication": True,
            "revision": WORLD_QA_DECK_REVISION,
            "cards": [],
            "reviews": {},
            "stats": {"pass": 0, "fail": 0, "unsure": 0, "reviewed": 0,
                      "total": 0},
            "globalReviews": {},
            "globalStats": {
                "pass": 0, "fail": 0, "unsure": 0, "reviewed": 0,
                "testers": 0, "total": 0,
            },
            "canRoute": False,
        }, cache_control="no-store")
    actor = clean_string(
        (record or {}).get("name"), MAX_NODE_NAME).strip().lower()
    can_route = False
    private_tasks = {}
    qa_org_bi = ""
    if actor:
        org_bi, org_row = await _org_row(env, "forkmesh")
        if org_row:
            role = await _org_role(env, org_bi, actor)
            permission = await _org_permission(env, org_bi, actor)
            can_route = (
                role in ("owner", "admin")
                or permission in ("maintain", "admin")
            )
            qa_org_bi = org_bi
    if qa_org_bi:
        task_rows = await d1_all(
            env,
            "SELECT task_id,department,team,qa_status,qa_reviewed_at,data "
            "FROM organization_tasks "
            "WHERE org_bi=? AND qa_requested_at>0 AND deleted_at=0 "
            "ORDER BY qa_requested_at DESC,task_id DESC LIMIT ?",
            qa_org_bi, WORLD_QA_MAX_CARDS,
        )
        for task_row in task_rows or []:
            task_id = str(task_row.get("task_id") or "").lower()
            if not re.fullmatch(r"[a-f0-9]{32}", task_id):
                continue
            try:
                task_data = await decrypt_row(env, task_row.get("data"))
            except Exception:
                task_data = None
            if not isinstance(task_data, dict):
                continue
            key = "task:" + task_id
            title = clean_string(task_data.get("title"), 160).strip()
            how_to_test = clean_string(
                task_data.get("howToTest"), 720).strip()
            if not title:
                continue
            if not how_to_test:
                how_to_test = (
                    "Follow the normal user flow and confirm the requested "
                    "behavior without regressions."
                )
            private_tasks[key] = {
                "row": task_row,
                "data": task_data,
                "title": title,
                "howToTest": how_to_test,
            }
            if key not in deck_keys:
                deck_cards.append((key, title, how_to_test))
                deck_keys.add(key)
    deck_cards = deck_cards[:WORLD_QA_MAX_CARDS]
    deck_keys = {item[0] for item in deck_cards}

    def include_private_task_reviews(global_reviews, global_stats):
        """Fold each task's authoritative first QA result into the snapshot."""
        for key, task in private_tasks.items():
            reviewed_at = max(
                0, int(task["row"].get("qa_reviewed_at") or 0))
            if not reviewed_at:
                continue
            verdict = {
                "passed": "pass",
                "failed": "fail",
                "unknown": "unsure",
            }.get(str(task["row"].get("qa_status") or ""))
            if not verdict:
                continue
            counts = {"pass": 0, "fail": 0, "unsure": 0, "total": 1}
            counts[verdict] = 1
            global_reviews[key] = counts
            global_stats[verdict] += 1
            global_stats["reviewed"] += 1
        global_stats["total"] = len(deck_cards)

    include_private_task_reviews(global_reviews, global_stats)
    if method == "POST":
        item_key = clean_string(data.get("key"), 80)
        if item_key not in deck_keys:
            return json_response({"ok": False, "error": "unknown_qa_item"},
                                 status=400)
        action = clean_string(data.get("action"), 24).lower()
        if action in ("route_todo", "route_issue"):
            if not can_route:
                return json_response(
                    {"ok": False, "error": "forbidden"}, status=403)
            card = next(
                (item for item in deck_cards if item[0] == item_key),
                None,
            )
            if not card:
                return json_response(
                    {"ok": False, "error": "unknown_qa_item"}, status=400)
            _, title, how_to_test = card
            now = int(Date.now())
            if action == "route_todo":
                task_suffix = re.sub(
                    r"[^a-z0-9-]+", "-", item_key.lower()).strip("-")
                task_key = ("task:qa-" + task_suffix)[:53].rstrip("-")
                peak = await d1_first(
                    env,
                    "SELECT COALESCE(MAX(priority),0) AS priority "
                    "FROM world_build_board_items",
                )
                priority = min(
                    64, max(1, int((peak or {}).get("priority") or 0) + 1))
                await d1_run(
                    env,
                    "INSERT INTO world_build_board_items "
                    "(item_key,kind,owner,repo,issue_number,title,priority,"
                    "updated_by_bi,updated_at,completed_at,completed_by_bi) "
                    "VALUES (?,'task','','',0,?,?,?,?,0,'') "
                    "ON CONFLICT(item_key) DO UPDATE SET "
                    "title=excluded.title,priority=excluded.priority,"
                    "updated_by_bi=excluded.updated_by_bi,"
                    "updated_at=excluded.updated_at,completed_at=0,"
                    "completed_by_bi=''",
                    task_key, title, priority, account_bi, now,
                )
                route_result = {
                    "target": "todo",
                    "key": task_key,
                }
            else:
                body = (
                    "QA follow-up requested from the 24-hour World deck.\n\n"
                    "How to test:\n" + how_to_test
                )
                queued, result = await _forkbot_enqueue_issue(
                    env,
                    "forkmesh",
                    "forkmesh",
                    "QA follow-up: " + title,
                    body,
                    actor,
                    source="qa-deck",
                    labels=["qa"],
                )
                if not queued:
                    return json_response(
                        {"ok": False, "error": str(result or "issue_failed")},
                        status=409,
                    )
                route_result = {
                    "target": "issues",
                    "number": int((result or {}).get("issueNumber") or 0),
                }
            await _audit_sensitive_action(
                env,
                actor,
                "world.qa_" + action,
                "world_qa_item",
                item_key,
                "success",
                route_result,
            )
        else:
            verdict = clean_string(data.get("verdict"), 12).lower()
            if verdict not in ("pass", "fail", "unsure"):
                return json_response(
                    {"ok": False, "error": "invalid_verdict"}, status=400)
            if item_key in private_tasks:
                if not qa_org_bi:
                    return json_response(
                        {"ok": False, "error": "qa_task_unavailable"},
                        status=403,
                    )
                task_id = item_key[5:]
                task_record = private_tasks[item_key]
                task_verdict = {
                    "pass": "passed",
                    "fail": "failed",
                    "unsure": "unknown",
                }[verdict]
                failure_reason = clean_string(
                    data.get("failureReason"), 1000).strip()
                failure_screenshot = str(
                    data.get("screenshot") or "").strip()
                if verdict == "fail" and not failure_reason:
                    return json_response(
                        {"ok": False, "error": "failure_reason_required"},
                        status=400,
                    )
                if failure_screenshot and (
                    verdict != "fail"
                    or len(failure_screenshot) > 480_000
                    or not re.fullmatch(
                        r"data:image/(?:png|jpeg|webp);base64,"
                        r"[A-Za-z0-9+/=]+",
                        failure_screenshot,
                    )
                ):
                    return json_response(
                        {"ok": False, "error": "invalid_qa_screenshot"},
                        status=400,
                    )
                if verdict != "fail":
                    failure_reason = ""
                    failure_screenshot = ""
                reviewed_at = int(Date.now())
                review_data = {
                    "reviewer": actor,
                    "failureReason": failure_reason,
                    "failureScreenshot": failure_screenshot,
                }
                await d1_run(
                    env,
                    "INSERT INTO organization_task_qa_reviews "
                    "(task_id,org_bi,reviewer_bi,verdict,data,reviewed_at) "
                    "VALUES (?,?,?,?,?,?) "
                    "ON CONFLICT(task_id,reviewer_bi) DO UPDATE SET "
                    "verdict=excluded.verdict,data=excluded.data,"
                    "reviewed_at=excluded.reviewed_at",
                    task_id, qa_org_bi, account_bi, task_verdict,
                    await encrypt_row(env, review_data),
                    reviewed_at,
                )
                task_data = dict(task_record["data"])
                task_data["qaReviewer"] = actor
                task_data["qaFailureReason"] = failure_reason
                task_data["qaFailureScreenshot"] = failure_screenshot
                await d1_run(
                    env,
                    "UPDATE organization_tasks SET qa_status=?,"
                    "qa_reviewer_bi=?,qa_reviewed_at=?,data=?,updated_at=? "
                    "WHERE org_bi=? AND task_id=? AND qa_requested_at>0 "
                    "AND deleted_at=0",
                    task_verdict, account_bi, reviewed_at,
                    await encrypt_row(env, task_data), reviewed_at,
                    qa_org_bi, task_id,
                )
                task_record["row"] = {
                    **task_record["row"],
                    "qa_status": task_verdict,
                    "qa_reviewed_at": reviewed_at,
                }
                task_record["data"] = task_data
                await _audit_sensitive_action(
                    env, actor, "organization.task_qa_reviewed",
                    "organization_task", task_id, "success",
                    {
                        "verdict": task_verdict,
                        "hasFailureReason": bool(failure_reason),
                        "hasScreenshot": bool(failure_screenshot),
                    },
                )
                # The verdict belongs on the task's own timeline, next to the
                # comments and check-ins that argued about it. Best-effort: a
                # missing timeline table must never fail a QA review.
                try:
                    await d1_run(
                        env,
                        "INSERT INTO organization_task_events "
                        "(event_id,task_id,org_bi,actor_bi,kind,data,"
                        "created_at) VALUES (?,?,?,?,'qa_reviewed',?,?)",
                        _ap_uuid(), task_id, qa_org_bi, account_bi,
                        await encrypt_row(env, {
                            "actor": actor,
                            "summary": task_verdict,
                            "mentions": [],
                        }),
                        reviewed_at,
                    )
                except Exception:
                    pass
                if task_verdict in ("passed", "failed"):
                    assigned = clean_string(
                        task_data.get("assignee"), 64).strip().lower()
                    try:
                        await notify_organization_task_update(
                            env, qa_org_bi, actor, task_id,
                            {
                                "id": task_id,
                                "title": task_data.get("title"),
                                "createdBy": task_data.get("createdBy"),
                                "assignee": assigned,
                                "assigneeKind": (
                                    "agent" if assigned == "agent"
                                    else "user" if assigned
                                    else "unassigned"
                                ),
                                "department": task_record["row"].get(
                                    "department"),
                                "team": task_record["row"].get("team"),
                                "status": "done",
                                "repository": task_data.get("repository"),
                            },
                            "qa " + task_verdict,
                        )
                    except Exception:
                        pass
            else:
                await d1_run(
                    env,
                    "INSERT INTO world_qa_reviews("
                    "account_bi,item_key,verdict,reviewed_at) VALUES(?,?,?,?) "
                    "ON CONFLICT(account_bi,item_key) DO UPDATE SET "
                    "verdict=excluded.verdict,reviewed_at=excluded.reviewed_at",
                    account_bi, item_key, verdict, int(Date.now()),
                )
        global_reviews, global_stats = await global_qa_snapshot()
        include_private_task_reviews(global_reviews, global_stats)
    rows = await d1_all(
        env,
        "SELECT item_key,verdict,reviewed_at FROM world_qa_reviews "
        "WHERE account_bi=? ORDER BY reviewed_at DESC LIMIT ?",
        account_bi, WORLD_QA_MAX_CARDS,
    )
    reviews = {}
    for row in rows:
        key = clean_string(row.get("item_key"), 80)
        verdict = clean_string(row.get("verdict"), 12).lower()
        if key in deck_keys and verdict in ("pass", "fail", "unsure"):
            reviews[key] = {
                "verdict": verdict,
                "reviewedAt": max(0, int(row.get("reviewed_at") or 0)),
            }
    if private_tasks:
        private_rows = await d1_all(
            env,
            "SELECT task_id,verdict,data,reviewed_at "
            "FROM organization_task_qa_reviews "
            "WHERE org_bi=? AND reviewer_bi=? "
            "ORDER BY reviewed_at DESC LIMIT ?",
            qa_org_bi, account_bi, WORLD_QA_MAX_CARDS,
        )
        for row in private_rows or []:
            key = "task:" + str(row.get("task_id") or "")
            task_verdict = str(row.get("verdict") or "")
            if key in private_tasks and task_verdict in (
                    "passed", "failed", "unknown"):
                try:
                    review_data = await decrypt_row(env, row.get("data"))
                except Exception:
                    review_data = {}
                if not isinstance(review_data, dict):
                    review_data = {}
                reviews[key] = {
                    "verdict": {
                        "passed": "pass",
                        "failed": "fail",
                        "unknown": "unsure",
                    }[task_verdict],
                    "reviewedAt": max(
                        0, int(row.get("reviewed_at") or 0)),
                    "failureReason": clean_string(
                        review_data.get("failureReason"), 1000).strip(),
                    "hasFailureScreenshot": bool(
                        review_data.get("failureScreenshot")),
                }
    stats = {
        "pass": sum(1 for item in reviews.values()
                    if item["verdict"] == "pass"),
        "fail": sum(1 for item in reviews.values()
                    if item["verdict"] == "fail"),
        "unsure": sum(1 for item in reviews.values()
                      if item["verdict"] == "unsure"),
        "reviewed": len(reviews),
        "total": len(deck_cards),
    }
    return json_response({
        "ok": True,
        "authenticated": True,
        "authorized": True,
        "requiredTeam": "",
        "requiresAuthentication": True,
        "revision": WORLD_QA_DECK_REVISION,
        "cards": [
            {
                "key": key,
                "title": title,
                "howToTest": how_to_test,
                "global": global_reviews.get(
                    key, {"pass": 0, "fail": 0, "unsure": 0, "total": 0}),
                "reviewedByAnyone": (
                    int(global_reviews.get(key, {}).get("total") or 0) > 0
                ),
                **reviews.get(key, {}),
                **({
                    "organizationTask": True,
                    "department": str(
                        private_tasks[key]["row"].get("department") or ""),
                    "team": str(
                        private_tasks[key]["row"].get("team") or ""),
                    "taskQaStatus": str(
                        private_tasks[key]["row"].get("qa_status")
                        or "unknown"),
                    "lastReviewer": clean_string(
                        private_tasks[key]["data"].get("qaReviewer"),
                        MAX_NODE_NAME,
                    ).strip().lower(),
                    "lastReviewedAt": max(
                        0,
                        int(private_tasks[key]["row"].get(
                            "qa_reviewed_at") or 0),
                    ),
                    "failureReason": clean_string(
                        private_tasks[key]["data"].get("qaFailureReason"),
                        1000,
                    ).strip(),
                    "hasFailureScreenshot": bool(
                        private_tasks[key]["data"].get(
                            "qaFailureScreenshot")),
                } if key in private_tasks else {}),
            }
            for key, title, how_to_test in deck_cards
        ],
        "reviews": reviews,
        "stats": stats,
        "globalReviews": global_reviews,
        "globalStats": global_stats,
        "canRoute": can_route,
        "canViewPrivateTasks": bool(qa_org_bi),
        **({"routeResult": route_result}
           if method == "POST" and "route_result" in locals() else {}),
    }, cache_control="no-store")
