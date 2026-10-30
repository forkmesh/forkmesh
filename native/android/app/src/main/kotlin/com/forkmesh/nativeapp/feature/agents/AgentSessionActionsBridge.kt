package com.forkmesh.nativeapp.feature.agents

import com.forkmesh.nativeapp.control.AgentSessionLookup
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.feature.inbox.AgentSessionActions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object AgentSessionActionsBridge : AgentSessionActions {
    private fun row(sessionId: String): UnifiedAgentRow {
        val control = AgentsFeature.control
        AgentSessionLookup.find(control.rows, sessionId)?.let { return it }
        control.refresh()
        return AgentSessionLookup.find(control.rows, sessionId) ?: error("That run is no longer listed. Open the Agents tab.")
    }

    override suspend fun continueRun(sessionId: String, text: String) = withContext(Dispatchers.IO) {
        val control = AgentsFeature.control
        val target = row(sessionId)
        if (text.isNotBlank()) {
            control.prompt(target, text)
        } else if (target.isDesktop) {
            control.continueRun(target)
        } else {
            error("Type what the agent should do next.")
        }
    }

    override suspend fun mergeAndCleanUp(sessionId: String) = withContext(Dispatchers.IO) {
        val target = row(sessionId)
        if (!target.isDesktop) error("Only a desktop run can be merged from here.")
        val client = AgentOpsFeature.client(target.desktopKey)
        val confirmation = client.prepareMerge(target.localId)
        client.merge(target.localId, confirmation.token, confirmation.typeToConfirm)
        AgentsFeature.control.refreshAsync()
    }

    override suspend fun retry(sessionId: String) = withContext(Dispatchers.IO) {
        val target = row(sessionId)
        if (target.isDesktop) {
            AgentsFeature.control.continueRun(target)
        } else {
            val queue = AgentsFeature.queue
            val session = queue.session(target.repoOwner, target.repoName, target.cloudSessionId, target.orgScope)
            queue.retry(target.repoOwner, target.repoName, session)
            AgentsFeature.control.refreshAsync()
        }
    }
}
