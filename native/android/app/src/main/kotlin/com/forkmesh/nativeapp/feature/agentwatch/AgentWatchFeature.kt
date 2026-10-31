package com.forkmesh.nativeapp.feature.agentwatch

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.agents.AgentRepoGroup
import com.forkmesh.nativeapp.agents.AgentsList
import com.forkmesh.nativeapp.agentwatch.AgentWatchTarget
import com.forkmesh.nativeapp.agentwatch.ControlWatchRoster
import com.forkmesh.nativeapp.agentwatch.deviceWatchTarget
import com.forkmesh.nativeapp.agentwatch.relayWatchTarget
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.control.AgentControlCenter
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.agents.AgentWatchEntry
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen

object AgentWatchFeature {
    fun install(app: App) {
        AgentWatchEntry.open = { navigator, title, targets -> open(navigator, title, targets) }
        AgentWatchEntry.openDevice = { navigator, repoName -> openDevice(navigator, repoName) }
        AgentWatchEntry.openControl = { navigator, title, rows -> openControl(navigator, title, rows) }
        AgentWatchEntry.openAll = { navigator, groups -> agentsTabScreen(groups)?.let { navigator.push(it) } }
    }

    private fun desktopKey(repoName: String, id: Int) = "${repoName.lowercase()}#$id"

    private fun listed(control: AgentControlCenter): Map<String, UnifiedAgentRow> =
        control.rows.filter { it.isDesktop && !it.outside && !it.elsewhere && it.localId > 0 }.associateBy { desktopKey(it.repoName, it.localId) }

    fun relayScreen(title: String, targets: List<Pair<Repository, AgentSession>>): Screen? {
        val api = Services.get<ApiService>() ?: return null
        if (targets.isEmpty()) return null
        val control = AgentsFeature.control
        val listed = listed(control)
        val streamed = ArrayList<UnifiedAgentRow>()
        val relay = ArrayList<AgentWatchTarget>()
        for ((repo, session) in targets.take(12)) {
            val row = listed[desktopKey(repo.name, session.id)]
            if (row != null) streamed += row else relay += relayWatchTarget(api, repo, session, AgentsFeature.ownerAccount(repo))
        }
        if (streamed.isEmpty()) return AgentWatchScreen(relay, title)
        return AgentWatchScreen(relay, title, debounce = 0.4, roster = ControlWatchRoster(control) { streamed })
    }

    fun agentsTabScreen(groups: List<AgentRepoGroup>): Screen? {
        val control = AgentsFeature.control
        val api = Services.get<ApiService>()
        val listed = listed(control)
        val relay = ArrayList<AgentWatchTarget>()
        if (api != null) {
            for (group in groups) {
                for (session in group.sessions) {
                    if (!session.isActive || listed.containsKey(desktopKey(group.repo.name, session.id))) continue
                    relay += relayWatchTarget(api, group.repo, session, AgentsFeature.ownerAccount(group.repo))
                }
            }
        }
        var replay = false
        if (relay.isEmpty() && control.rows.none { it.isActive }) {
            if (api != null) {
                val (_, chosen) = AgentsList.watchChoice(groups)
                for ((repo, session) in chosen) relay += relayWatchTarget(api, repo, session, AgentsFeature.ownerAccount(repo))
            }
            if (relay.isEmpty() && control.rows.isEmpty()) return null
            replay = true
        }
        val replayRows = replay && relay.isEmpty()
        val roster = ControlWatchRoster(control) { center ->
            val running = center.rows.filter { it.isActive }
            if (running.isEmpty() && replayRows) center.rows.take(3) else running
        }
        return AgentWatchScreen(relay.take(12), if (replay) "Replay agents" else "Watch agents", debounce = 0.4, roster = roster)
    }

    fun deviceScreen(repoName: String): Screen {
        val sessions = AgentsFeature.sessions
        val running = sessions.lastRun?.isComplete == false
        return AgentWatchScreen(listOf(deviceWatchTarget(sessions, repoName)), if (running) "Watch agent" else "Replay agent", debounce = 0.12)
    }

    fun controlScreen(title: String, rows: List<UnifiedAgentRow>): Screen {
        val chosen = rows.take(12)
        return AgentWatchScreen(emptyList(), title, debounce = 0.4, roster = ControlWatchRoster(AgentsFeature.control) { chosen })
    }

    private fun open(navigator: Navigator, title: String, targets: List<Pair<Repository, AgentSession>>) {
        relayScreen(title, targets)?.let { navigator.push(it) }
    }

    private fun openDevice(navigator: Navigator, repoName: String) {
        navigator.push(deviceScreen(repoName))
    }

    private fun openControl(navigator: Navigator, title: String, rows: List<UnifiedAgentRow>) {
        if (rows.isNotEmpty()) navigator.push(controlScreen(title, rows))
    }
}
