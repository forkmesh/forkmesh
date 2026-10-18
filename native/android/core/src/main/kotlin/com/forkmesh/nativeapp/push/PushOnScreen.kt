package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.inbox.InboxRoutes
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.Route

data class PushScreen(val tab: AppTab, val route: Route? = null, val covered: Boolean = false)

private val TASK_HREF = Regex("^/dashboard/tasks/?\\?(?:.*&)?task=([^&#]+)")

fun pingOnScreen(ping: ForkNotification, screen: PushScreen): Boolean {
    if (screen.covered || pingAlarm(ping.kind, ping.meta).isNotEmpty()) return false
    val target = InboxRoutes.route(ping.kind, ping.href, ping.meta, ping.repo)
    if (target is Route.Agent && screen.tab == AppTab.Agents) {
        when (val route = screen.route) {
            null, is Route.Agents, Route.AllAgents, is Route.AgentWatch -> return true
            is Route.Agent -> return sameAgent(route.id, target.id)
            else -> Unit
        }
    }
    if (screen.tab == AppTab.Inbox && screen.route == null) return true
    val task = TASK_HREF.find(ping.href.trim())?.groupValues?.get(1)
    if (task != null && screen.route == Route.Task(task)) return true
    return target != null && target == screen.route
}

private fun sameAgent(a: String, b: String): Boolean = a.isNotEmpty() && a.substringAfterLast(':') == b.substringAfterLast(':')
