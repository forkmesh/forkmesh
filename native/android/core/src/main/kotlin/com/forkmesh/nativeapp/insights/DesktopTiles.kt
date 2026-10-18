package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

data class DesktopTileEntry(val id: String, val title: String, val group: String, val hint: String)

data class DesktopTilesReply(val tiles: Map<String, StatsTileData>, val sampledAt: Long) {
    val offered: Set<String> get() = tiles.keys
}

object DesktopTiles {
    const val PREFIX = "desktop."
    const val MAX_SERIES = 60

    fun boardId(desktopId: String): String = PREFIX + desktopId

    fun isDesktopTile(id: String): Boolean = id.startsWith(PREFIX)

    private val groupIcons = mapOf(
        "Workspace" to "inventory_2_outlined",
        "Ratchet" to "trending_up",
        "Issues & pulls" to "bug_report",
        "Chat & pings" to "chat_bubble_outline",
        "Agents" to "auto_awesome",
        "Agent tools" to "build",
        "Actions" to "play_circle_outline",
        "Realtime" to "bolt",
        "Network & logs" to "lan",
        "This machine" to "computer",
        "Market & rewards" to "storefront",
        "Users" to "people",
    )

    val entries: List<DesktopTileEntry> = listOf(
        DesktopTileEntry("repos", "Repositories", "Workspace", "Repositories on this node, split by how they are served"),
        DesktopTileEntry("repo-commits", "Commits", "Workspace", "Commits on the open repository's served branch, per week for the last year (from the last mirror advert)"),
        DesktopTileEntry("repo-merges", "Merges", "Workspace", "Merge commits on the open repository, per week"),
        DesktopTileEntry("repo-branches", "Branches", "Workspace", "Local branches the open repository's mirror holds"),
        DesktopTileEntry("repo-worktrees", "Worktrees", "Workspace", "Worktrees the open repository's mirror advertises, with its registered pulls"),
        DesktopTileEntry("repo-size", "Repository size", "Workspace", "Tracked bytes in the open repository, per day, from its generated statistics"),
        DesktopTileEntry("repo-lines", "Lines of code", "Workspace", "Tracked lines in the open repository, per day"),
        DesktopTileEntry("repo-files", "Tracked files", "Workspace", "Tracked files in the open repository, per day"),
        DesktopTileEntry("scm-changes", "Working-tree changes", "Workspace", "Uncommitted rows Source Control lists for the open repository"),
        DesktopTileEntry("teammates", "Special agents", "Workspace", "AI special agents configured for the open repository"),
        DesktopTileEntry("ratchet", "Ratchet mode", "Ratchet", "Whether Ratchet Mode is on for the open repository: commits that grow the tracked bytes past the day's ceiling are refused"),
        DesktopTileEntry("ratchet-ceiling", "Ratchet ceiling", "Ratchet", "Today's size ceiling, with the repository's tracked bytes per day beneath it"),
        DesktopTileEntry("ratchet-headroom", "Ratchet headroom", "Ratchet", "Bytes left under the ceiling before a commit is refused, per day"),
        DesktopTileEntry("issues-open", "Open issues", "Issues & pulls", "Open issues in the open repository, with the last 30 days of the burn-up"),
        DesktopTileEntry("issues-closed", "Closed issues", "Issues & pulls", "Closed issues in the open repository, over 30 days"),
        DesktopTileEntry("issues-priority", "Issues by priority", "Issues & pulls", "Open issues grouped by priority band"),
        DesktopTileEntry("issues-labels", "Issues by label", "Issues & pulls", "Labels on open issues, most used first"),
        DesktopTileEntry("issues-assigned", "Assigned issues", "Issues & pulls", "Open issues with an assignee, by assignee"),
        DesktopTileEntry("issues-progress", "Issue progress", "Issues & pulls", "Average reported progress across open issues, by quarter"),
        DesktopTileEntry("issues-votes", "Issue votes", "Issues & pulls", "Uptime-credit votes across open issues, by issue"),
        DesktopTileEntry("issues-bounties", "Issue bounties", "Issues & pulls", "Bounties posted on open issues, by issue"),
        DesktopTileEntry("issues-rewards", "Reward estimates", "Issues & pulls", "Contributor reward estimates on issues, by status"),
        DesktopTileEntry("pulls", "Pull requests", "Issues & pulls", "Pull requests in the open repository: open, merged, closed"),
        DesktopTileEntry("pulls-conflicts", "Conflicting pulls", "Issues & pulls", "Open pull requests whose last conflict probe found conflicts"),
        DesktopTileEntry("discussions", "Discussions", "Issues & pulls", "Discussions in the open repository, by status, with comments"),
        DesktopTileEntry("chat-unread", "Unread messages", "Chat & pings", "Unread chat messages, by channel"),
        DesktopTileEntry("chat-channels", "Channels", "Chat & pings", "Chat channels this node follows"),
        DesktopTileEntry("pings", "Pings", "Chat & pings", "Pings received, per day over two weeks"),
        DesktopTileEntry("pings-kinds", "Pings by kind", "Chat & pings", "Pings grouped by what raised them"),
        DesktopTileEntry("roster-versions", "App versions", "Chat & pings", "Roster members by the ForkMesh version they advertise"),
        DesktopTileEntry("tasks", "Organization tasks", "Chat & pings", "Tasks loaded from the organization board"),
        DesktopTileEntry("agents", "Agent sessions", "Agents", "Every agent session on this node, by status"),
        DesktopTileEntry("agents-running", "Running agents", "Agents", "Sessions running right now, with sessions started per day over two weeks"),
        DesktopTileEntry("agent-success", "Agent success rate", "Agents", "Finished sessions that succeeded or merged"),
        DesktopTileEntry("agent-tokens", "Tokens used", "Agents", "Tokens across every agent session, per day"),
        DesktopTileEntry("agent-spend", "Agent spend", "Agents", "Estimated cost of agent sessions, per day; the note carries the provider bills when known"),
        DesktopTileEntry("agent-turns", "Agent turns", "Agents", "Model turns across every session, per day"),
        DesktopTileEntry("agent-runtime", "Agent run time", "Agents", "Time agents have spent running, with the last twenty sessions"),
        DesktopTileEntry("agent-prs", "Agent pull requests", "Agents", "Sessions that opened a pull request, and how many merged"),
        DesktopTileEntry("agent-errors", "Agent errors", "Agents", "Sessions that ended with an error, with the newest message"),
        DesktopTileEntry("agents-providers", "Agents by provider", "Agents", "Sessions grouped by the provider that ran them"),
        DesktopTileEntry("agents-models", "Agents by model", "Agents", "Sessions grouped by model"),
        DesktopTileEntry("agent-skills", "Skills used", "Agents", "Skills sessions reported using, most used first"),
        DesktopTileEntry("agent-grades", "Task grades", "Agents", "Grades given to finished agent tasks"),
        DesktopTileEntry("agent-teammates", "Special agent sessions", "Agents", "Sessions started by AI special agents, by special agent"),
        DesktopTileEntry("claude-usage", "Claude usage", "Agents", "Claude subscription usage for the 5-hour and weekly windows"),
        DesktopTileEntry("tool-calls", "Tool calls", "Agent tools", "Agent tool calls recorded by the efficiency ledger, per five minutes over the last two hours"),
        DesktopTileEntry("tool-errors", "Tool errors", "Agent tools", "Tool calls that returned an error, by family"),
        DesktopTileEntry("tokens-saved", "Tokens saved", "Agent tools", "Context tokens the tool layer avoided injecting"),
        DesktopTileEntry("context-injected", "Context injected", "Agent tools", "Tokens tool results actually put into context"),
        DesktopTileEntry("tool-baseline", "Baseline tokens", "Agent tools", "What the naive alternative to each measured call would have cost, against what was injected"),
        DesktopTileEntry("tool-latency", "Tool latency", "Agent tools", "Average measured tool call latency"),
        DesktopTileEntry("tool-coverage", "Tool coverage", "Agent tools", "Tool families observed, with calls per family"),
        DesktopTileEntry("tool-runtimes", "Runtime tools", "Agent tools", "Runtime tool names the agents called, most used first"),
        DesktopTileEntry("tool-while-off", "Calls while off", "Agent tools", "Calls that arrived while their family was switched off"),
        DesktopTileEntry("improvement-signals", "Improvement signals", "Agent tools", "Potential tool-use improvements from the realtime log, by kind"),
        DesktopTileEntry("improvement-stake", "Context at stake", "Agent tools", "Context tokens the logged improvement patterns cost, by family"),
        DesktopTileEntry("guardrail-blocked", "Blocked tool calls", "Agent tools", "Tool calls the guardrails blocked, per hour over the last day"),
        DesktopTileEntry("guardrail-decisions", "Guardrail decisions", "Agent tools", "Approvals, denials and agents stopped by the guardrails"),
        DesktopTileEntry("workflows", "Workflows", "Actions", "Workflow files in the open repository, by trigger"),
        DesktopTileEntry("action-runs", "Action runs", "Actions", "Workflow runs on this node, by status"),
        DesktopTileEntry("action-history", "Runs per day", "Actions", "Workflow runs started per day over two weeks"),
        DesktopTileEntry("action-failed", "Failed runs", "Actions", "Workflow runs that failed, per day over two weeks"),
        DesktopTileEntry("action-success", "Action success rate", "Actions", "Finished workflow runs that succeeded"),
        DesktopTileEntry("action-duration", "Action duration", "Actions", "Average finished run time, with the last twenty runs"),
        DesktopTileEntry("action-wait", "Action queue wait", "Actions", "Average time from a run being created to it starting, with the last twenty runs"),
        DesktopTileEntry("action-workflow-times", "Time per workflow", "Actions", "Average finished run time of each workflow"),
        DesktopTileEntry("action-slowest", "Slowest run", "Actions", "The longest finished workflow run on record"),
        DesktopTileEntry("action-last", "Last run time", "Actions", "How long the most recent finished run took"),
        DesktopTileEntry("action-steps", "Action steps", "Actions", "Workflow steps executed across every run, by outcome"),
        DesktopTileEntry("action-pull-runs", "Pull request runs", "Actions", "Runs triggered by pull requests, and how many were held for a first-time author"),
        DesktopTileEntry("agents-live", "Agents running now", "Realtime", "Sessions running at this moment, sampled every refresh"),
        DesktopTileEntry("agent-queue", "Agent queue", "Realtime", "Sessions queued or waiting to run, sampled every refresh"),
        DesktopTileEntry("agent-context", "Context in use", "Realtime", "How full each running session's context window is"),
        DesktopTileEntry("agent-background", "Agent subprocesses", "Realtime", "Background processes running sessions still own, sampled every refresh"),
        DesktopTileEntry("compilers", "Compilers", "Realtime", "Compiler processes under the agents, with their memory"),
        DesktopTileEntry("tool-in-flight", "Tool calls in flight", "Realtime", "Tool calls started and not yet answered, sampled every refresh"),
        DesktopTileEntry("action-queue", "Action queue", "Realtime", "Workflow runs awaiting approval, queued or running right now"),
        DesktopTileEntry("relay-latency", "Relay latency", "Realtime", "Round trip to the active relay, sampled every refresh, with every relay measured"),
        DesktopTileEntry("network-requests", "Network requests", "Realtime", "HTTP requests this session by destination, live: green is allowed by a firewall rule, red is denied by one, amber has no decision yet, blue stays on this machine or the firewall is off"),
        DesktopTileEntry("background-tasks", "Background tasks", "Realtime", "Open background work tickets, sampled every refresh"),
        DesktopTileEntry("loop-busy", "Event loop busy", "Realtime", "Share of time the GUI event dispatcher was busy, with wakeups per second"),
        DesktopTileEntry("nodes-online", "Nodes online", "Network & logs", "Roster members with a live link, by platform"),
        DesktopTileEntry("linked-nodes", "Linked nodes", "Network & logs", "Nodes linked to your account"),
        DesktopTileEntry("network-catalog", "Network catalog", "Network & logs", "Relays, nodes and hosts the Network page lists"),
        DesktopTileEntry("servers", "Relays configured", "Network & logs", "Relay servers saved on this node, and which is active"),
        DesktopTileEntry("services", "Service status", "Network & logs", "Website and desktop service checks that are operational"),
        DesktopTileEntry("web-alerts", "Web alerts", "Network & logs", "Alerts the website raised for this account"),
        DesktopTileEntry("firewall", "Request firewall", "Network & logs", "Outbound requests the request firewall allowed or blocked"),
        DesktopTileEntry("uptime", "Connection uptime", "Network & logs", "How long this session has been connected, and the lifetime total"),
        DesktopTileEntry("log-lines", "Log lines", "Network & logs", "Network log lines this session, by category"),
        DesktopTileEntry("log-history", "Log history", "Network & logs", "Network log lines per day over the 30-day archive"),
        DesktopTileEntry("stalls", "UI stalls", "Network & logs", "Times the GUI thread stalled this session"),
        DesktopTileEntry("cpu", "CPU", "This machine", "This app's CPU share, sampled while the page is open"),
        DesktopTileEntry("memory", "Memory", "This machine", "This app's resident memory, sampled while the page is open"),
        DesktopTileEntry("host-memory", "Host memory", "This machine", "Memory in use across the whole machine"),
        DesktopTileEntry("swap", "Swap", "This machine", "Swap in use across the whole machine"),
        DesktopTileEntry("disk", "Disk free", "This machine", "Free space on the volume holding the open repository"),
        DesktopTileEntry("open-files", "Open files", "This machine", "File descriptors this app holds against its soft limit"),
        DesktopTileEntry("threads", "Threads", "This machine", "Threads in this process"),
        DesktopTileEntry("app-uptime", "App uptime", "This machine", "Time since this app instance launched"),
        DesktopTileEntry("restarts", "Restarts", "This machine", "Rebuild-and-restart and update cycles this install has timed"),
        DesktopTileEntry("restart-time", "Restart time", "This machine", "How long recent restarts took, from the click to the relaunched window's first paint"),
        DesktopTileEntry("loop-lag", "Event-loop lag", "This machine", "95th percentile GUI event-loop lag from the Qt runtime diagnostics"),
        DesktopTileEntry("events-per-second", "Events per second", "This machine", "Qt events dispatched per second"),
        DesktopTileEntry("widgets", "Widgets", "This machine", "Live QWidget count, with the QObject total"),
        DesktopTileEntry("windows", "Windows", "This machine", "Top-level windows, and how many are visible"),
        DesktopTileEntry("timers", "Qt timers", "This machine", "Active Qt timers against every registered timer"),
        DesktopTileEntry("thread-pool", "Thread pool", "This machine", "Global thread pool workers busy, against its maximum"),
        DesktopTileEntry("diff-cache", "Diff cache", "This machine", "Memory held by cached branch diffs"),
        DesktopTileEntry("packages", "Package installs", "Market & rewards", "Installs and downloads across marketplace packages"),
        DesktopTileEntry("release-downloads", "Release downloads", "Market & rewards", "Downloads across the open repository's release assets"),
        DesktopTileEntry("rewards", "Contributor rewards", "Market & rewards", "Reward records fetched for the reward pool, by status"),
        DesktopTileEntry("ledger", "Ledger records", "Market & rewards", "Payable ledger records from the relay, and this machine's own"),
        DesktopTileEntry("wallets", "Wallet balances", "Market & rewards", "Public balances of the wallets this machine names"),
        DesktopTileEntry("payouts", "Settled payouts", "Market & rewards", "Rewards paid from this machine and proven on-chain"),
        DesktopTileEntry("users", "Users", "Users", "Accounts in the directory, by activity bucket"),
        DesktopTileEntry("users-activity", "User activity", "Users", "Commits, pulls, issues and discussions across every account"),
        DesktopTileEntry("users-world", "World steps", "Users", "Steps taken in World across every account, with office visits"),
    )

    val catalog: List<StatsTileCatalogEntry> = entries.map {
        StatsTileCatalogEntry(boardId(it.id), it.title, it.group, it.hint, groupIcons[it.group] ?: "computer")
    }

    val ids: Set<String> = catalog.map { it.id }.toSet()

    val wide: Set<String> = setOf("issues-open", "agents", "tool-calls", "cpu", "repo-commits", "action-history", "pings", "agent-tokens", "log-history")
        .map(::boardId).toSet()

    val defaults: List<StatsTilePlacement> = catalog.map { StatsTilePlacement(it.id, width = if (it.id in wide) 2 else 1) }

    fun fromJson(json: Map<String, Any?>): DesktopTilesReply {
        val tiles = LinkedHashMap<String, StatsTileData>()
        for (tile in json.maps("tiles")) {
            val id = tile.str("id")
            if (id.isNotEmpty()) tiles[boardId(id)] = tileData(tile)
        }
        return DesktopTilesReply(tiles, json.long("sampledAt"))
    }

    fun tileData(json: Map<String, Any?>): StatsTileData {
        val series = json.obj("series")
        val values = series.list("values").map { (it as? Number)?.toDouble()?.takeIf(Double::isFinite) ?: 0.0 }.takeLast(MAX_SERIES)
        val labels = series.strings("labels").takeLast(values.size)
        return StatsTileData(
            value = json.str("value"),
            note = json.str("note"),
            series = StatsTileSeries(labels, values, series.bool("categorical")),
            available = json.optBool("available") ?: true,
            status = tone(json.str("status")),
            trend = when (json.str("trend")) {
                "up" -> StatsTrendPolarity.UpIsGood
                "down" -> StatsTrendPolarity.DownIsGood
                else -> StatsTrendPolarity.Neutral
            },
        )
    }

    fun tone(wire: String): StatsTone = when (wire) {
        "good" -> StatsTone.Good
        "warning" -> StatsTone.Warning
        "critical" -> StatsTone.Critical
        else -> StatsTone.None
    }
}
