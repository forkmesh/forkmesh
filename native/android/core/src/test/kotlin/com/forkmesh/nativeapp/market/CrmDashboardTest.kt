package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.testing.assertEq

class CrmDashboardTest {
    private val day = 86_400_000L
    private val now = 100 * day

    private fun contact(stage: String, value: Long = 0, created: Long = 0, contacted: Long = 0) =
        CrmContact(id = "a".repeat(32), name = "x", flow = "customers", stage = stage, value = value, createdAt = created, lastContactedAt = contacted)

    fun testCountsOpenWonAndDueContacts() {
        val rows = listOf(
            contact("lead", value = 500, created = now - day),
            contact("qualified", value = 300, created = now - 30 * day, contacted = now - 20 * day),
            contact("customer", value = 1000, contacted = now - day),
            contact("lost", value = 900),
            contact("lead", contacted = now - 2 * day),
        )
        val board = CrmDashboard.of(rows, now)
        assertEq(5, board.contacts)
        assertEq(1, board.newThisWeek)
        assertEq(2, board.openDeals)
        assertEq(800L, board.openValue)
        assertEq(1, board.wonDeals)
        assertEq(1000L, board.wonValue)
        assertEq(1, board.neverContacted)
        assertEq(1, board.quiet)
        assertEq(2, board.toContact)
    }

    fun testGreeting() {
        assertEq("Good morning", CrmDashboard.greeting(8))
        assertEq("Good afternoon", CrmDashboard.greeting(13))
        assertEq("Good evening", CrmDashboard.greeting(21))
        assertEq("Good evening", CrmDashboard.greeting(2))
    }

    fun testPipelineFlowPrefersTheActiveThenTheBusiest() {
        val data = CrmOrganizationData.fromJson(
            mapOf(
                "contacts" to listOf(
                    mapOf("id" to "a".repeat(32), "flow" to "hiring", "stage" to "applied"),
                    mapOf("id" to "b".repeat(32), "flow" to "hiring", "stage" to "applied"),
                ),
                "settings" to mapOf(
                    "flows" to listOf(
                        mapOf("id" to "customers", "name" to "Customers", "stages" to listOf("lead")),
                        mapOf("id" to "hiring", "name" to "Hiring", "stages" to listOf("applied")),
                    ),
                ),
            ),
        )
        assertEq("hiring", CrmDashboard.pipelineFlow(data, null)?.id)
        assertEq("customers", CrmDashboard.pipelineFlow(data, data.flow("customers"))?.id)
    }

    fun testSectionsFollowTheTab() {
        assertEq(CrmSection.CONTACTS, CrmSection.of(CrmTab.Pipeline("customers")))
        assertEq(CrmSection.ACTIVITY, CrmSection.of(CrmTab.Mail))
        assertEq(CrmDealState.WON, CrmDealState.of("committed"))
        assertEq(CrmDealState.ENDED, CrmDealState.of("declined"))
        assertEq(listOf("Home", "Contacts", "Pipeline", "Activity", "More"), CrmSection.entries.map { it.title })
    }
}
