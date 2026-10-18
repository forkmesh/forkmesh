package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.PullTab
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.expect

class PushOnScreenTest {
    private val done = ForkNotification(id = "a1", kind = "desktop_ping", title = "Agent #7", meta = mapOf("sessionId" to "7"))

    fun testAnAgentDonePingIsHiddenOnTheAgentsList() {
        expect(pingOnScreen(done, PushScreen(AppTab.Agents)))
        expect(pingOnScreen(done, PushScreen(AppTab.Agents, Route.AllAgents)))
    }

    fun testAnAgentDonePingIsHiddenOnThatAgentOnly() {
        expect(pingOnScreen(done, PushScreen(AppTab.Agents, Route.Agent("desktop:7"))))
        expect(!pingOnScreen(done, PushScreen(AppTab.Agents, Route.Agent("desktop:8"))))
    }

    fun testAnAgentDonePingShowsElsewhereOrWhenCovered() {
        expect(!pingOnScreen(done, PushScreen(AppTab.Code)))
        expect(!pingOnScreen(done, PushScreen(AppTab.Agents, covered = true)))
    }

    fun testAPingIsHiddenOnTheInboxList() {
        val ping = ForkNotification(id = "p1", kind = "release_published", title = "Release published")
        expect(pingOnScreen(ping, PushScreen(AppTab.Inbox)))
        expect(!pingOnScreen(ping, PushScreen(AppTab.Inbox, Route.PingSettings)))
        expect(!pingOnScreen(ping, PushScreen(AppTab.Chat)))
    }

    fun testAPingIsHiddenOnTheRecordItLinksTo() {
        val ping = ForkNotification(id = "p2", kind = "pull_comment", title = "New comment", href = "/acme/site/pulls/12")
        val repo = RepoRef("acme", "site")
        expect(pingOnScreen(ping, PushScreen(AppTab.Code, Route.Pull(repo, 12, PullTab.Conversation))))
        expect(!pingOnScreen(ping, PushScreen(AppTab.Code, Route.Pull(repo, 13, PullTab.Conversation))))
    }

    fun testATaskPingIsHiddenOnThatTask() {
        val ping = ForkNotification(id = "p3", kind = "organization_task_update", title = "Task done", href = "/dashboard/tasks/?task=t9")
        expect(pingOnScreen(ping, PushScreen(AppTab.Inbox, Route.Task("t9"))))
        expect(!pingOnScreen(ping, PushScreen(AppTab.Inbox, Route.Task("t8"))))
    }

    fun testAnAlarmAlwaysShows() {
        val alarm = ForkNotification(id = "p4", kind = "desktop_ping", title = "tower lost power", meta = mapOf("alarm" to "power"))
        expect(!pingOnScreen(alarm, PushScreen(AppTab.Inbox)))
    }
}
