package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DesktopCommandContractTest {
    fun testCanonicalStringAndAllowList() {
        assertEq(
            "forkmesh-desktop-command-v1\nme\napp\npull.merge\npull:7\n123",
            DesktopCommandContract.canonical("me", "app", "pull.merge", "pull:7", "123"),
        )
        expect(DesktopCommandContract.isValid("pull.merge", DesktopCommandContract.pullTarget(7)))
        expect(!DesktopCommandContract.isValid("pull.merge", "pull:0"))
        expect(!DesktopCommandContract.isValid("pull.merge", "run:7"))
        expect(DesktopCommandContract.isValid("action.rerun", DesktopCommandContract.runTarget(42)))
        expect(DesktopCommandContract.isValid("release.publish", DesktopCommandContract.releaseTarget("v1.2.4+b")))
        expect(!DesktopCommandContract.isValid("release.publish", "release:-bad"))
        expect(!DesktopCommandContract.isValid("shell.run", "pull:1"))
        assertEq(11, DesktopCommandContract.commands.size)
        assertEq("/api/repo/me/app/desktop-commands", DesktopCommandContract.path("me", "app"))
    }

    fun testRouteProbeAnswers() {
        assertEq(true, DesktopCommandContract.routeAnswered(403, "not_authorized"))
        assertEq(true, DesktopCommandContract.routeAnswered(400, "unsupported_command"))
        assertEq(false, DesktopCommandContract.routeAnswered(404, ""))
        assertEq(false, DesktopCommandContract.routeAnswered(405, ""))
        assertEq(null, DesktopCommandContract.routeAnswered(500, ""))
        assertEq("This server does not take desktop requests yet.", DesktopCommandContract.errorMessage(404, ""))
        assertEq("Desktop request was not queued (HTTP 502).", DesktopCommandContract.errorMessage(502, ""))
        assertEq("That request was already sent.", DesktopCommandContract.errorMessage(409, "replayed_request"))
    }

    fun testAvailabilityRechecksAfterFifteenMinutes() {
        var now = 1_000L
        val availability = DesktopCommandAvailability { now }
        var probes = 0
        expect(!availability.refresh { probes++; false })
        expect(!availability.refresh { probes++; true })
        assertEq(1, probes)
        now += DesktopCommandAvailability.RECHECK_MS
        expect(availability.refresh { probes++; true })
        expect(availability.refresh { probes++; false })
        assertEq(2, probes)
        availability.reset()
        expect(!availability.available)
        expect(!availability.refresh { probes++; null })
        expect(availability.refresh { probes++; true })
    }
}
