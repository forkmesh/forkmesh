package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.testing.assertEq

class PingLeadTest {
    fun testAPingWithAnActorLeadsWithWhoAndWhere() {
        val ping = ForkNotification("1", "issue_comment", "New comment for acme/widgets", "Looks good to me", repo = "acme/widgets", actor = "bob", meta = mapOf("number" to 42))
        assertEq("bob · acme/widgets #42" to "Looks good to me", PingLead.of(ping))
    }

    fun testWithoutTextTheEventLabelIsTheSecondLine() {
        val ping = ForkNotification("1", "issue_opened", "Issue submitted for acme/widgets", "", repo = "acme/widgets", actor = "bob")
        assertEq("bob · acme/widgets" to "Issue submitted", PingLead.of(ping))
        assertEq("carol" to "Tip", PingLead.of(ForkNotification("2", "tip_sent", "Tip", actor = "carol")))
    }

    fun testAPingWithoutAnActorKeepsItsLabelWithTheNumber() {
        val release = ForkNotification("3", "release_published", "Release v2 in acme/widgets", "Notes", repo = "acme/widgets", meta = mapOf("number" to "7"))
        assertEq("#7 Release v2" to "Notes", PingLead.of(release))
        assertEq("Notification" to "", PingLead.of(ForkNotification("4", "x", "  ")))
        assertEq("Credits refilled" to "", PingLead.of(ForkNotification("5", "credits_refilled", "Credits refilled", meta = mapOf("number" to 3))))
    }

    fun testTheRepositoryPrefixIsDropped() {
        assertEq("New issue", PingLead.label("acme/widgets: New issue", "Acme/Widgets"))
        assertEq("Starred", PingLead.label("acme/widgets Starred", "acme/widgets"))
        assertEq("Mirror request on other/repo", PingLead.label("Mirror request on other/repo", "acme/widgets"))
    }
}
