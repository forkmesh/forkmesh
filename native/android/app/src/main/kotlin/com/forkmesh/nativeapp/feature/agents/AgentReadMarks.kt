package com.forkmesh.nativeapp.feature.agents

import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentReadFacts
import com.forkmesh.nativeapp.agents.AgentReadState
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.control.liveDesktopRow
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.platform.Prefs

internal object AgentReadMarks {
    private var prefs: Prefs? = null
    private var state by mutableStateOf(AgentReadState())

    @Composable
    fun Load() {
        ensure(LocalContext.current)
    }

    @Synchronized
    fun ensure(context: Context) {
        if (prefs != null) return
        val store = Prefs(context.applicationContext)
        val stored = store.getLong(AgentReadState.BASELINE_KEY)
        val loaded = AgentReadState.load(stored, store.getString(AgentReadState.SEEN_KEY), System.currentTimeMillis())
        if (stored <= 0) store.putLong(AgentReadState.BASELINE_KEY, loaded.baselineMs)
        state = loaded
        prefs = store
    }

    fun unread(facts: AgentReadFacts?): Boolean = state.isUnread(facts)

    fun unread(row: UnifiedAgentRow): Boolean = unread(AgentReadState.facts(row))

    fun unread(repoName: String, session: AgentSession): Boolean = unread(desktopFacts(repoName, session))

    fun unread(session: AgentLaunchSession): Boolean = unread(AgentReadState.facts(session))

    fun desktopFacts(repoName: String, session: AgentSession): AgentReadFacts? = AgentReadState.facts(
        repoName,
        session,
        liveDesktopRow(AgentsFeature.control.rows, repoName, session.id),
    )

    fun open(facts: AgentReadFacts?) {
        if (facts == null) return
        val next = state.markRead(facts)
        if (next != state) {
            state = next
            prefs?.putString(AgentReadState.SEEN_KEY, next.encodeSeen())
        }
        if (facts.desktopId > 0) AgentsFeature.control.markReadAsync(facts.desktopId)
    }

    fun open(row: UnifiedAgentRow) = open(AgentReadState.facts(row))

    fun open(repoName: String, session: AgentSession) = open(desktopFacts(repoName, session))

    fun open(session: AgentLaunchSession) = open(AgentReadState.facts(session))
}
