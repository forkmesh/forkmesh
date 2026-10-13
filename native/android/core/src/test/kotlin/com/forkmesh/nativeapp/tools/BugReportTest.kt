package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.bodyJson
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class BugReportTest {
    private val build = AppBuild(version = "0.1.0+1", platform = "android", osVersion = "Android 16 (API 36)", mode = "debug")

    private class Relay(status: Int = 201, body: Any? = mapOf("ok" to true, "id" to "a".repeat(32))) {
        val transport = FakeTransport { jsonResponse(body, status) }
        val api = ApiService(TestSettings(serverUrl = "ws://relay.test/ws"), transport)
        val submitted: List<Map<String, Any?>> get() = transport.requests.map { it.bodyJson() }
    }

    private val layer = BugButtonGeometry(layerWidth = 800f, layerHeight = 600f)

    fun testTheControlIsSmallAndRestsAtItsDefaultPlacement() {
        expect(BugButtonGeometry.SIZE_DP <= 48f)
        val at = layer.restingAt(anchor = 0.72, onLeft = false)
        assertEq(800f - 42f - 12f, at.x)
        assertEq(12f + (0.72 * 534).toFloat(), at.y)
    }

    fun testTheControlMovesOutOfTheWayAndStaysWhereItIsPut() {
        val before = layer.restingAt(0.72, onLeft = false)
        val placement = layer.released(layer.dragged(before, -500f, -250f))
        expect(placement.onLeft)
        expect(placement.anchor < 0.72)
        val after = layer.restingAt(placement.anchor, placement.onLeft)
        expect(after.x < before.x)
        expect(after.y < before.y)
    }

    fun testADragThatDoesNotCrossTheMidlineKeepsItsEdge() {
        val before = layer.restingAt(0.72, onLeft = false)
        val placement = layer.released(layer.dragged(before, -120f, -200f))
        expect(!placement.onLeft)
        val after = layer.restingAt(placement.anchor, placement.onLeft)
        assertEq(before.x, after.x)
        expect(after.y < before.y)
    }

    fun testARememberedPlacementIsRestoredOnTheNextBuild() {
        val at = layer.restingAt(anchor = 0.1, onLeft = true)
        expect(at.x < 400f)
        expect(at.y < 600f * 0.25f)
    }

    fun testTheControlStaysInsideItsLayerWhenDraggedPastAnEdge() {
        val at = layer.dragged(layer.restingAt(0.72, onLeft = false), 500f, 4000f)
        expect(at.x >= 0f && at.x + BugButtonGeometry.SIZE_DP <= 800f)
        expect(at.y >= 0f && at.y + BugButtonGeometry.SIZE_DP <= 600f)
        val placement = layer.released(at)
        assertEq(1.0, placement.anchor)
        val corner = layer.dragged(layer.restingAt(0.0, onLeft = true), -500f, -4000f)
        assertEq(BugButtonPoint(12f, 12f), corner)
        assertEq(0.0, layer.released(corner).anchor)
    }

    fun testTheSafeAreaKeepsTheControlOffTheSystemBars() {
        val barred = BugButtonGeometry(layerWidth = 400f, layerHeight = 800f, safeTop = 24f, safeBottom = 48f)
        assertEq(800f - 24f - 48f - 42f - 24f, barred.travel)
        assertEq(36f, barred.restingAt(0.0, onLeft = true).y)
        assertEq(36f + barred.travel, barred.restingAt(1.0, onLeft = true).y)
        assertEq(36f, barred.dragged(BugButtonPoint(12f, 100f), 0f, -500f).y)
    }

    fun testALayerShorterThanTheControlStillHasFiniteTravel() {
        val squashed = BugButtonGeometry(layerWidth = 30f, layerHeight = 30f)
        assertEq(1f, squashed.travel)
        val at = squashed.dragged(squashed.restingAt(0.5, onLeft = false), 10f, 10f)
        expect(!at.x.isNaN() && !at.y.isNaN())
        expect(!squashed.released(at).anchor.isNaN())
    }

    fun testTheComposerSendsWhatWasTypedAndItsContext() {
        val relay = Relay()
        val step = ReportBugDraft()
            .withMessage("The agent transcript stops mid-run.")
            .withSeverity("crash")
            .submit(apiAvailable = true)
        expect(step.send)
        expect(step.draft.busy)

        val result = sendBugReport(relay.api, step.draft, screen = "agents", build = build)

        expect(result.ok)
        val sent = relay.submitted.single()
        assertEq("The agent transcript stops mid-run.", sent["message"])
        assertEq("crash", sent["severity"])
        assertEq("agents", sent["screen"])
        assertEq("mobile", sent["source"])
        assertEq("android", sent["platform"])
        assertEq("0.1.0+1", sent["appVersion"])
        assertEq("Android 16 (API 36)", sent["osVersion"])
        expect(!sent.containsKey("contact"))
        expect(!sent.containsKey("device"))
    }

    fun testSeverityDefaultsToNormalSoOneFieldIsEnough() {
        val relay = Relay()
        val step = ReportBugDraft().withMessage("Just this one line.").submit(apiAvailable = true)
        sendBugReport(relay.api, step.draft, screen = "", build = build)
        assertEq("normal", relay.submitted.single()["severity"])
        assertEq(listOf("low", "normal", "high", "crash"), ReportBugCopy.SEVERITIES.map { it.value })
        assertEq(listOf("Minor", "Normal", "Serious", "Crash"), ReportBugCopy.SEVERITIES.map { it.label })
    }

    fun testTheComposerRefusesToSendAnEmptyDescription() {
        val step = ReportBugDraft().withMessage("   ").submit(apiAvailable = true)
        expect(!step.send)
        assertEq("Describe what went wrong.", step.draft.problem)
        expect(!step.draft.busy)
        assertEq("", step.draft.withMessage("Now it crashes").problem)
        assertEq("Describe what went wrong.", step.draft.withMessage("  ").problem)
    }

    fun testARefusalKeepsTheSheetOpenWithTheTextIntact() {
        val relay = Relay(status = 429, body = mapOf("error" to "rate_limited"))
        val typed = "Everything breaks when I rotate the phone."
        val step = ReportBugDraft().withMessage(typed).withContact("me@example.com").submit(apiAvailable = true)

        val result = sendBugReport(relay.api, step.draft, screen = "code", build = build)
        val shown = step.draft.refused(result.message)

        expect(!result.ok)
        assertEq(typed, shown.message)
        assertEq("me@example.com", shown.contact)
        expect(shown.problem.contains("several reports"))
        expect(!shown.busy)
    }

    fun testReportingWithoutAnApiSaysSo() {
        val step = ReportBugDraft().withMessage("Broken.").submit(apiAvailable = false)
        expect(!step.send)
        assertEq("Reporting is unavailable in this build.", step.draft.problem)
        expect(!step.draft.busy)
    }

    fun testABusyComposerIgnoresASecondSubmit() {
        val sending = ReportBugDraft().withMessage("Broken.").submit(apiAvailable = true).draft
        val again = sending.submit(apiAvailable = true)
        expect(!again.send)
        assertEq(sending, again.draft)
    }

    fun testTheComposerNamesTheContextItAttaches() {
        assertEq(
            "Attached: ForkMesh 0.1.0+1 · android · chat · no account.",
            ReportBugCopy.attached(build, screen = "chat", signedIn = false),
        )
        assertEq("Attached: ForkMesh 0.1.0+1 · android · your account.", ReportBugCopy.attached(build, screen = "", signedIn = true))
        assertEq("Reply to (optional)", ReportBugCopy.contactLabel(signedIn = true))
        assertEq("Email or handle (optional)", ReportBugCopy.contactLabel(signedIn = false))
        assertEq("Only if you want the reply somewhere else", ReportBugCopy.contactHint(signedIn = true))
        assertEq("Only if you want a reply", ReportBugCopy.contactHint(signedIn = false))
    }

    fun testTheReportedBuildHasThePubspecShape() {
        assertEq("0.1.0+1", AppBuild.version("0.1.0", 1))
        assertEq("Android 16 (API 36)", AppBuild.androidOsVersion("16", 36))
        assertEq("debug", AppBuild.mode(debuggable = true))
        assertEq("release", AppBuild.mode(debuggable = false))
    }
}
