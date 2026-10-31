package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.market.PackageMonitors
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class FleetLightsTest {
    private val report = StatusReport.fromJson(
        mapOf(
            "systems" to listOf(
                mapOf("id" to "website", "label" to "Website", "status" to "operational"),
                mapOf("id" to "core:n1", "status" to "operational", "resources" to mapOf("cpu" to 12.5, "memory" to 81, "disk" to 140)),
                mapOf("id" to "core:n2", "status" to "down", "resources" to mapOf("cpu" to null, "memory" to -1)),
                mapOf("id" to "runner:r1", "status" to "operational", "resources" to mapOf("cpu" to 50)),
                mapOf("id" to "core:", "status" to "operational"),
            ),
            "current" to emptyMap<String, Any?>(),
        ),
    )

    fun testCoreLightsCarryTheirCpuMemoryAndDisk() {
        val lights = FleetLight.lights(report, null, onlyProblems = false)
        assertEq(listOf("core:n1", "core:n2", "runner:r1"), lights.map { it.id })
        assertEq(NodeResources(12.5, 81.0, 100.0), lights[0].resources)
        assertEq(listOf<Double?>(12.5, 81.0, 100.0), lights[0].resources?.values)
        assertEq("Core node n1, operational, CPU 13%, memory 81%, disk 100%", lights[0].label)
        assertEq(null, lights[1].resources)
        assertEq("Core node n2, has an issue", lights[1].label)
        assertEq(null, lights[2].resources)
        assertEq("Runner r1, signed in", lights[2].label)
        assertEq("CPU unknown, memory 40%, disk unknown", NodeResources(null, 40.0, null).label)
        assertEq(null, NodeResources.of(null, -3.0, Double.NaN))
    }

    fun testOnlyProblemsAndMonitorsFilterTheLights() {
        assertEq(listOf("core:n2"), FleetLight.lights(report, null, onlyProblems = true).map { it.id })
        val cores = listOf(PackageMonitors(prefixes = listOf("core:")))
        assertEq(listOf("core:n1", "core:n2"), FleetLight.lights(report, cores, onlyProblems = false).map { it.id })
        assertEq(emptyList<FleetLight>(), FleetLight.lights(null, null, onlyProblems = false))
    }

    fun testStatusRowCarriesTheFleetAndKeepsServicesApart() {
        val row = StatusDots.build(true, report, null, StatusPageSettings())
        assertEq(listOf("core:n1", "core:n2", "runner:r1"), row.fleet.map { it.id })
        assertEq(listOf("website"), row.services.map { it.id })
        assertEq("Website, operational", row.servicesDescription)
        expect(row.dots.any { it.id == "core:n1" })
    }
}
