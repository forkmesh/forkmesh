package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.market.AccountCatalogEntry
import com.forkmesh.nativeapp.market.PackageMonitors
import com.forkmesh.nativeapp.market.StatusMonitor
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class StatusDotsTest {
    private val report = StatusReport.fromJson(
        mapOf(
            "systems" to listOf(
                mapOf("id" to "website", "label" to "Website", "status" to "operational"),
                mapOf("id" to "api", "label" to "API", "status" to "degraded", "reason" to "slow"),
                mapOf("id" to "core:n1", "label" to "n1", "status" to "operational"),
                mapOf("id" to "runner:r1", "label" to "r1", "status" to "down"),
                mapOf("id" to "mirror:vultr1", "label" to "vultr1", "status" to "operational"),
                mapOf("id" to "durable_objects", "label" to "Durables", "status" to "future"),
                mapOf("id" to "email", "label" to "Email", "status" to ""),
            ),
            "current" to emptyMap<String, Any?>(),
        ),
    )

    private val catalog = listOf(
        AccountCatalogEntry("status-page", "Status Page"),
        AccountCatalogEntry("status-check-website", "Status: Website", dependencies = listOf("status-page"), monitors = PackageMonitors(listOf("website", "desktop_website"))),
        AccountCatalogEntry("status-check-api", "Status: API", dependencies = listOf("status-page"), monitors = PackageMonitors(listOf("api"))),
        AccountCatalogEntry("status-check-cores", "Status: Core Nodes", dependencies = listOf("status-page"), monitors = PackageMonitors(prefixes = listOf("core:"))),
        AccountCatalogEntry("status-check-mirrors", "Status: Mirrors", dependencies = listOf("status-page"), monitors = PackageMonitors(prefixes = listOf("mirror:"))),
        AccountCatalogEntry("status-check-durable-objects", "Status: DO", dependencies = listOf("status-page"), monitors = PackageMonitors(listOf("durable_objects"))),
    ).associateBy { it.id }

    fun testDotsFollowTheInstalledCheckPackages() {
        val off = setOf("status-check-mirrors")
        val row = StatusDots.rowFor(report, catalog, { it !in off }, mapOf("onlyProblems" to false, "refreshMinutes" to "15"))
        assertEq(listOf("website", "api", "core:n1"), row.dots.map { it.id })
        assertEq(listOf(ServiceState.Operational, ServiceState.Degraded, ServiceState.Operational), row.dots.map { it.state })
        assertEq("slow", row.dots[1].reason)
        expect(row.troubled)
        assertEq(StatusDots.TROUBLED_REFRESH_MS, row.refreshMs)
        assertEq("Website, operational; API, degraded; n1, operational", row.description)
    }

    fun testNothingWhileStatusPageIsRemoved() {
        val monitor = StatusMonitor("a".repeat(32), "Blog", "https://blog.example.com/")
        val row = StatusDots.rowFor(report, catalog, { it != "status-page" }, emptyMap(), listOf(StatusDots.customDot(monitor, true)))
        assertEq(StatusDotRow.EMPTY, row)
    }

    fun testUnknownCatalogShowsEverySystem() {
        val row = StatusDots.rowFor(report, emptyMap(), { true }, emptyMap())
        assertEq(listOf("website", "api", "core:n1", "runner:r1", "mirror:vultr1", "email"), row.dots.map { it.id })
        assertEq(ServiceState.Unknown, row.dots.last().state)
    }

    fun testCustomMonitorsAndOnlyProblems() {
        val blog = StatusMonitor("a".repeat(32), "Blog", "https://blog.example.com/")
        val shop = StatusMonitor("b".repeat(32), "Shop", "https://shop.example.com/")
        val custom = listOf(StatusDots.customDot(blog, true), StatusDots.customDot(shop, false))
        assertEq("custom:" + "a".repeat(32), custom[0].id)
        assertEq(ServiceState.Down, custom[1].state)
        assertEq(ServiceState.Unknown, StatusDots.customDot(blog, null).state)
        val noChecks = catalog.keys.filter { it != "status-page" }.toSet()
        val all = StatusDots.rowFor(report, catalog, { it !in noChecks }, emptyMap(), custom)
        assertEq(listOf("Blog", "Shop"), all.dots.map { it.label })
        expect(all.troubled)
        val problems = StatusDots.rowFor(report, catalog, { true }, mapOf("onlyProblems" to true), custom)
        assertEq(listOf("api", "custom:" + "b".repeat(32)), problems.dots.map { it.id })
    }

    fun testHealthyRefreshFollowsTheSetting() {
        val healthy = StatusReport.fromJson(mapOf("systems" to listOf(mapOf("id" to "website", "label" to "Website", "status" to "operational"))))
        assertEq(5 * 60_000L, StatusDots.rowFor(healthy, catalog, { true }, mapOf("refreshMinutes" to "5")).refreshMs)
        assertEq(15 * 60_000L, StatusDots.rowFor(healthy, catalog, { true }, mapOf("refreshMinutes" to "7")).refreshMs)
        assertEq(30 * 60_000L, StatusDots.rowFor(healthy, catalog, { true }, mapOf("refreshMinutes" to 30)).refreshMs)
        val hiddenButTroubled = StatusDots.rowFor(report, catalog, { it != "status-check-api" }, mapOf("onlyProblems" to true, "refreshMinutes" to "30"))
        assertEq(emptyList<String>(), hiddenButTroubled.dots.map { it.id })
        assertEq(30 * 60_000L, hiddenButTroubled.refreshMs)
        val grey = StatusReport.fromJson(mapOf("systems" to listOf(mapOf("id" to "website", "status" to "operational"), mapOf("id" to "email", "status" to ""))))
        assertEq(StatusDots.TROUBLED_REFRESH_MS, StatusDots.rowFor(grey, emptyMap(), { true }, mapOf("refreshMinutes" to "30")).refreshMs)
        assertEq(StatusPageSettings(onlyProblems = true, refreshMinutes = 2), StatusPageSettings.of(mapOf("onlyProblems" to true, "refreshMinutes" to "2")))
    }

    fun testProbeIsRedOnUnreachableAddresses() {
        expect(!HttpMonitorProbe.ok("not a url"))
        expect(!HttpMonitorProbe.ok("http://[::1]:1/"))
    }
}
