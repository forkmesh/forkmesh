package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class BackgroundLinkPlanTest {
    private val next = LinkProject("app.next.forkmesh.com", "ForkMesh Next", signedIn = true, enabled = true)
    private val prod = LinkProject("app.forkmesh.com", "ForkMesh", signedIn = true, enabled = true)

    fun testOneSocketPerEnabledSignedInProject() {
        val plan = LinkPlan.of(listOf(next, prod, prod), LinkConditions())
        expect(plan.runService)
        expect(plan.live)
        assertEq(listOf(next.id, prod.id), plan.projectIds)
    }

    fun testSignedOutOrSwitchedOffProjectsAreLeftOut() {
        val plan = LinkPlan.of(listOf(next.copy(enabled = false), prod.copy(signedIn = false)), LinkConditions())
        assertEq(LinkPlan.OFF, plan)
        assertEq(listOf(prod.id), LinkPlan.of(listOf(next.copy(enabled = false), prod), LinkConditions()).projectIds)
    }

    fun testNoNotificationPermissionStillListensForCallsOnly() {
        val plan = LinkPlan.of(listOf(next), LinkConditions(notificationsAllowed = false))
        expect(plan.runService)
        expect(plan.callsOnly)
        expect(!plan.mesh)
        expect(plan.live)
        assertEq(listOf(next.id), plan.projectIds)
        assertEq(LinkPlan.CALLS_TITLE, plan.title)
        assertEq("Listening for calls on ForkMesh Next", plan.statusText(setOf(next.id)))
        assertEq(LinkPlan.OFF, LinkPlan.of(emptyList(), LinkConditions(notificationsAllowed = false)))
        assertEq(LinkPlan.OFF, LinkPlan.of(listOf(next.copy(enabled = false)), LinkConditions(notificationsAllowed = false)))
        expect(!LinkPlan.of(listOf(next), LinkConditions()).callsOnly)
    }

    fun testSignedOutPhonesStillListenOnTheMesh() {
        for (projects in listOf(emptyList(), listOf(prod.copy(signedIn = false)))) {
            val plan = LinkPlan.of(projects, LinkConditions(networkAvailable = false))
            expect(plan.runService)
            expect(plan.mesh)
            expect(plan.projectIds.isEmpty())
            assertEq(LinkPlan.MESH_TITLE, plan.title)
            assertEq(LinkPlan.MESH_STATUS, plan.statusText(emptySet()))
        }
        val both = LinkPlan.of(listOf(next), LinkConditions())
        expect(both.mesh)
        assertEq("Connected for pings", both.title)
    }

    fun testNoNetworkOrDozeKeepsTheServiceButPausesSockets() {
        val offline = LinkPlan.of(listOf(next), LinkConditions(networkAvailable = false))
        expect(offline.runService)
        expect(!offline.live)
        val dozing = LinkPlan.of(listOf(next), LinkConditions(deviceIdle = true))
        expect(dozing.runService)
        expect(!dozing.live)
        assertEq("Waiting for a network connection", offline.statusText(emptySet()))
    }

    fun testStatusText() {
        val plan = LinkPlan.of(listOf(next, prod), LinkConditions())
        assertEq("Listening for pings on ForkMesh Next and ForkMesh", plan.statusText(setOf(next.id, prod.id)))
        assertEq("Connecting to ForkMesh Next and ForkMesh", plan.statusText(emptySet()))
        assertEq("Connected to 1 of 2 projects", plan.statusText(setOf(prod.id)))
        assertEq("", LinkPlan.OFF.statusText(emptySet()))
    }

    fun testDiffOpensClosesAndKeeps() {
        val plan = LinkPlan.of(listOf(next, prod), LinkConditions())
        val diff = diffLinks(setOf(prod.id, "old.example"), plan)
        assertEq(listOf(next.id), diff.open)
        assertEq(listOf("old.example"), diff.close)
        assertEq(listOf(prod.id), diff.keep)
        assertEq(listOf("old.example"), diffLinks(setOf("old.example"), LinkPlan.OFF).close)
    }

    fun testKeysArePerProject() {
        expect(BackgroundLinkKeys.enabled(next.id) != BackgroundLinkKeys.enabled(prod.id))
        assertEq("push/background/_", BackgroundLinkKeys.enabled(""))
        assertEq("push/ledger/app.forkmesh.com", BackgroundLinkKeys.ledger(prod.id))
    }
}
