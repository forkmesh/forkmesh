package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AppStoreStatusTest {
    private val example = """
        {
          "format": "forkmesh-app-store-status-v1",
          "generatedAt": 1760000000000,
          "app": {"id": "6752000000", "bundleId": "com.forkmesh.app"},
          "attention": ["1.4.0 was rejected: Apple's message is in App Store Connect -> App Review"],
          "versions": [
            {"version": "1.4.0", "build": "19", "releaseType": "AFTER_APPROVAL",
             "createdAt": "2026-09-16T09:45:00Z",
             "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}
          ],
          "submissions": [
            {"id": "s1", "submittedAt": "2026-10-08T18:29:00Z",
             "status": {"state": "UNRESOLVED_ISSUES", "label": "Unresolved issues", "tone": "action"},
             "items": [{"name": "1.4.0", "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}]}
          ],
          "downloads": {
            "available": true, "note": "", "since": "2026-08",
            "total": {"downloads": 18, "redownloads": 2, "updates": 7},
            "recent": [{"days": 7, "downloads": 1, "redownloads": 0, "updates": 2},
                       {"days": 30, "downloads": 104, "redownloads": 0, "updates": 2}]
          },
          "builds": [
            {"version": "1.4.0", "build": "19", "uploadedAt": "2026-10-05T12:51:00Z", "expired": false,
             "processing": {"state": "VALID", "label": "Valid", "tone": "good"},
             "internal": {"state": "IN_BETA_TESTING", "label": "In beta testing", "tone": "good"},
             "external": {"state": "WAITING_FOR_BETA_REVIEW", "label": "Waiting for beta review", "tone": "pending"},
             "betaReview": {"state": "", "label": "Not submitted", "tone": "neutral"},
             "groups": ["forkmesh-team"]}
          ],
          "groups": [
            {"name": "outside", "internal": false, "testers": 1, "allBuilds": false,
             "publicLinkEnabled": true, "publicLink": "https://testflight.apple.com/join/x",
             "publicLinkLimit": 0, "feedbackEnabled": true}
          ],
          "feedback": [
            {"kind": "screenshot", "createdAt": "2026-09-01T03:06:00Z", "from": "t@example.com",
             "device": "iPhone16_2 27.0", "comment": "Show time as 5:45pm",
             "screenshots": [{"id": "3f1c2a9b8d7e6f5a4b3c2d1e", "width": 1179, "height": 2556}]}
          ],
          "reviews": [
            {"createdAt": "2026-10-01T10:00:00Z", "rating": 5, "title": "Great", "territory": "USA",
             "body": "Works well"}
          ],
          "unavailable": {"feedback": "ERROR: App Store Connect GET answered 404"}
        }
    """.trimIndent()

    private fun reply(snapshot: String) = Json.parseObject(
        """{"ok": true, "actor": "jett", "published": true, "publishedAt": 1760000500000, "snapshot": $snapshot}""",
    )

    fun testDecodesTheDocumentedExample() {
        val parsed = AppStoreStatusReply.fromJson(reply(example))
        assertEq("jett", parsed.actor)
        expect(parsed.published)
        assertEq(1760000500000L, parsed.publishedAt)
        val snap = parsed.snapshot!!
        assertEq(AppStoreStatus.FORMAT, snap.format)
        assertEq(1760000000000L, snap.generatedAt)
        assertEq("6752000000", snap.appId)
        assertEq("com.forkmesh.app", snap.bundleId)
        assertEq(listOf("1.4.0 was rejected: Apple's message is in App Store Connect -> App Review"), snap.attention)

        val version = snap.versions.single()
        assertEq("1.4.0", version.version)
        assertEq("19", version.build)
        assertEq("AFTER_APPROVAL", version.releaseType)
        assertEq(AppStoreState("REJECTED", "Rejected", AppStoreTone.ACTION), version.status)

        val submission = snap.submissions.single()
        assertEq("s1", submission.id)
        assertEq("Unresolved issues", submission.status.label)
        assertEq(listOf(AppStoreSubmissionItem("1.4.0", AppStoreState("REJECTED", "Rejected", AppStoreTone.ACTION))), submission.items)

        val downloads = snap.downloads
        expect(downloads.available)
        assertEq("Total since 2026-08", downloads.totalLabel)
        assertEq(AppStoreCounts(18, 2, 7), downloads.total)
        assertEq(listOf(AppStoreSpan(7, AppStoreCounts(1, 0, 2)), AppStoreSpan(30, AppStoreCounts(104, 0, 2))), downloads.recent)
        assertEq("Last 7 days", downloads.recent.first().label)

        val build = snap.builds.single()
        assertEq("1.4.0 (19)", AppStoreStatus.buildName(build.version, build.build))
        expect(!build.expired)
        assertEq(AppStoreTone.GOOD, build.processing.tone)
        assertEq(AppStoreTone.GOOD, build.internal.tone)
        assertEq(AppStoreTone.PENDING, build.external.tone)
        assertEq(AppStoreState("", "Not submitted", AppStoreTone.NEUTRAL), build.betaReview)
        assertEq(listOf("forkmesh-team"), build.groups)

        val group = snap.groups.single()
        assertEq(AppStoreGroup("outside", false, 1, false, true, "https://testflight.apple.com/join/x", 0, true), group)
        assertEq("https://testflight.apple.com/join/x", group.openableLink)

        assertEq(
            AppStoreFeedback(
                "screenshot", "2026-09-01T03:06:00Z", "t@example.com", "iPhone16_2 27.0", "Show time as 5:45pm",
                listOf(AppStoreScreenshot("3f1c2a9b8d7e6f5a4b3c2d1e", 1179, 2556)),
            ),
            snap.feedback.single(),
        )
        assertEq(AppStoreReview("2026-10-01T10:00:00Z", 5, "Great", "USA", "Works well"), snap.reviews.single())
        assertEq("ERROR: App Store Connect GET answered 404", snap.unavailable(AppStoreStatus.FEEDBACK))
        assertEq("", snap.unavailable(AppStoreStatus.REVIEWS))
    }

    fun testNothingPublishedYetHasNoSnapshot() {
        val parsed = AppStoreStatusReply.fromJson(Json.parseObject("""{"ok": true, "actor": "jett", "published": false, "publishedAt": 0, "snapshot": null}"""))
        expect(!parsed.published)
        assertEq(null, parsed.snapshot)
        assertEq(null, AppStoreStatusReply.fromJson(emptyMap()).snapshot)
    }

    fun testMissingFieldsDefaultAndUnknownToneIsNeutral() {
        val snap = AppStoreStatusReply.fromJson(
            reply(
                """{"versions": [{"status": {"state": "SOMETHING_NEW", "tone": "purple"}}, {}],
                   "builds": [{"processing": "VALID", "groups": ["", "a"]}],
                   "groups": [{"name": "b", "publicLinkEnabled": false, "publicLink": "https://x"}, {"name": "a", "internal": true}],
                   "reviews": [{"rating": 9}], "feedback": [{}], "downloads": "nope",
                   "unavailable": {"builds": "", "reviews": "x"}, "attention": ["", "  look  "]}""",
            ),
        ).snapshot!!
        assertEq(AppStoreState("SOMETHING_NEW", "SOMETHING_NEW", AppStoreTone.NEUTRAL), snap.versions[0].status)
        assertEq(AppStoreState("", "Unknown", AppStoreTone.NEUTRAL), snap.versions[1].status)
        assertEq("", snap.versions[1].version)
        assertEq(AppStoreState(), snap.builds.single().processing)
        assertEq(listOf("a"), snap.builds.single().groups)
        assertEq(listOf("a", "b"), snap.orderedGroups.map { it.name })
        assertEq(null, snap.groups.first().openableLink)
        assertEq(5, snap.reviews.single().rating)
        assertEq("anonymous", snap.feedback.single().from)
        expect(!snap.feedback.single().crash)
        assertEq(AppStoreDownloads(), snap.downloads)
        assertEq("Total", snap.downloads.totalLabel)
        assertEq(mapOf("reviews" to "x"), snap.unavailable)
        assertEq(listOf("look"), snap.attention)
        assertEq("", snap.bundleId)
        assertEq(AppStoreTone.ACTION, AppStoreTone.of(" Action "))
        assertEq(AppStoreTone.NEUTRAL, AppStoreTone.of(null))
    }

    fun testFormatsStampsStarsAndBuildNames() {
        assertEq(1760000000000L, AppStoreStatus.epochMs("2025-10-09T08:53:20Z"))
        assertEq(AppStoreStatus.epochMs("2026-10-08T18:29:00Z"), AppStoreStatus.epochMs("2026-10-08T11:29:00-07:00"))
        assertEq(null, AppStoreStatus.epochMs(""))
        assertEq(null, AppStoreStatus.epochMs("yesterday"))
        assertEq("★★★☆☆", AppStoreStatus.stars(3))
        assertEq("☆☆☆☆☆", AppStoreStatus.stars(-1))
        assertEq("1.4.0", AppStoreStatus.buildName("1.4.0", ""))
        assertEq("Build 7", AppStoreStatus.buildName("", "7"))
        assertEq("Unknown", AppStoreStatus.buildName("", ""))
    }

    fun testIsANativePackageDestinationBehindTheGate() {
        assertEq(PackagePhoneSurface.Native("appStoreStatus"), MarketPackage(id = AppStoreStatus.PACKAGE_ID, name = "App Store Status", webRoute = AppStoreStatus.PATH).phoneSurface)
        assertEq(AppStoreStatus.PACKAGE_ID, PackageGate.ROUTE_KINDS[Route.Kind.AppStoreStatus])
        assertEq(AppStoreStatus.PACKAGE_ID, PackageGate.HUB_ITEMS[Route.Kind.AppStoreStatus.raw])
        expect(AppStoreStatus.PACKAGE_ID in PackageGate.GATED)
        val removed = PackageSurfaces(setOf(AppStoreStatus.PACKAGE_ID))
        expect(!removed.showsRoute(Route.AppStoreStatus) && Route.Kind.AppStoreStatus.raw in removed.hubItems)
    }

    fun testLoadsThroughThePackagesClient() {
        var seen = ""
        val client = PackagesClient { method, path, _, _ ->
            seen = "$method $path"
            ApiJsonResult(200, reply(example))
        }
        val parsed = AppStoreStatus.load(client)
        assertEq("GET /api/app-store-status", seen)
        assertEq("com.forkmesh.app", parsed.snapshot?.bundleId)
        val failing = PackagesClient { _, _, _, _ -> ApiJsonResult(401, mapOf("error" to "invalid_session")) }
        val error = runCatching { AppStoreStatus.load(failing) }.exceptionOrNull()
        assertEq("Sign in again to continue.", error?.message)
    }

    fun testFeedbackScreenshotsKeepValidIdsOnly() {
        val snap = AppStoreStatusReply.fromJson(
            reply(
                """{"feedback": [{"screenshots": [
                     {"id": "3f1c2a9b8d7e6f5a4b3c2d1e", "width": 1179, "height": -3},
                     {"id": "../../etc/passwd"}, {"id": "AAAAAAAAAAAAAAAAAAAAAAAA"}, "junk",
                     {"id": "aaaaaaaaaaaaaaaaaaaaaaaa"}, {"id": "bbbbbbbbbbbbbbbbbbbbbbbb"},
                     {"id": "cccccccccccccccccccccccc"}, {"id": "dddddddddddddddddddddddd"}]}, {}]}""",
            ),
        ).snapshot!!
        val shots = snap.feedback[0].screenshots
        assertEq(
            listOf("3f1c2a9b8d7e6f5a4b3c2d1e", "aaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbb", "cccccccccccccccccccccccc"),
            shots.map { it.id },
        )
        assertEq(AppStoreScreenshot("3f1c2a9b8d7e6f5a4b3c2d1e", 1179, 0), shots[0])
        assertEq(null, shots[0].aspectRatio)
        assertEq(0.5f, AppStoreScreenshot("aaaaaaaaaaaaaaaaaaaaaaaa", 100, 200).aspectRatio)
        assertEq(emptyList<AppStoreScreenshot>(), snap.feedback[1].screenshots)
    }

    fun testScreenshotsComeFromThePackageApi() {
        val png = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13)
        val encoded = java.util.Base64.getEncoder().encodeToString(png)
        val asked = mutableListOf<String>()
        val client = PackagesClient { method, path, _, _ ->
            asked += "$method $path"
            when {
                path.endsWith("/3f1c2a9b8d7e6f5a4b3c2d1e") ->
                    ApiJsonResult(200, mapOf("ok" to true, "mime" to "image/png", "data" to encoded))
                path.endsWith("/bbbbbbbbbbbbbbbbbbbbbbbb") ->
                    ApiJsonResult(200, mapOf("ok" to true, "mime" to "image/jpeg", "data" to encoded))
                else -> ApiJsonResult(404, mapOf("error" to "screenshot_not_found"))
            }
        }
        assertEq("/api/app-store-status/screenshots/3f1c2a9b8d7e6f5a4b3c2d1e", AppStoreStatus.screenshotPath("3f1c2a9b8d7e6f5a4b3c2d1e"))
        expect(png.contentEquals(AppStoreStatus.loadScreenshot(client, AppStoreScreenshot("3f1c2a9b8d7e6f5a4b3c2d1e"))))
        assertEq(listOf("GET /api/app-store-status/screenshots/3f1c2a9b8d7e6f5a4b3c2d1e"), asked)
        val missing = runCatching { AppStoreStatus.loadScreenshot(client, AppStoreScreenshot("aaaaaaaaaaaaaaaaaaaaaaaa")) }.exceptionOrNull()
        expect(missing is AppStoreStatus.ScreenshotMissingException)
        assertEq(AppStoreStatus.SCREENSHOT_MISSING, missing?.message)
        val lying = runCatching { AppStoreStatus.loadScreenshot(client, AppStoreScreenshot("bbbbbbbbbbbbbbbbbbbbbbbb")) }.exceptionOrNull()
        expect(lying is PackagesException)
        assertEq(null, AppStoreStatus.screenshotBytes(mapOf("mime" to "image/png", "data" to "%%%")))
        assertEq(null, AppStoreStatus.screenshotBytes(mapOf("mime" to "image/gif", "data" to encoded)))
        assertEq("Screenshot", AppStoreStatus.screenshotTitle(0, 1))
        assertEq("Screenshot 2 of 3", AppStoreStatus.screenshotTitle(1, 3))
    }

    private val withoutScreenshots get() = example.replace(
        "\"screenshots\": [{\"id\": \"3f1c2a9b8d7e6f5a4b3c2d1e\", \"width\": 1179, \"height\": 2556}]",
        "\"screenshots\": []",
    )

    private fun snapshotOf(text: String) = AppStoreSnapshot.fromJson(Json.parseObject(text))

    fun testSnapshotLacksScreenshotsOnlyWhenNoReportHasOne() {
        expect(withoutScreenshots != example)
        expect(!snapshotOf(example).lacksScreenshots)
        expect(snapshotOf(withoutScreenshots).lacksScreenshots)
        expect(!snapshotOf("""{"feedback": [{"kind": "crash"}]}""").lacksScreenshots)
        expect(!snapshotOf("{}").lacksScreenshots)
    }

    fun testAsksTheDesktopWhenTheServerSnapshotPredatesScreenshots() {
        var desktopCalls = 0
        val server = Json.parseObject("""{"ok": true, "publishedAt": 1760000300000, "snapshot": $withoutScreenshots}""")
        val fresh = Json.parseObject("""{"ok": true, "source": "desktop", "publishedAt": 1760000400000, "snapshot": $example}""")
        val describe: (Throwable) -> String = { it.message.orEmpty() }
        val stale = AppStoreStatusLoader.load({ server }, { desktopCalls++; fresh }, describe)
        assertEq(1, desktopCalls)
        assertEq(true, stale.reply?.fromDesktop)
        assertEq(listOf("3f1c2a9b8d7e6f5a4b3c2d1e"), stale.reply?.snapshot?.feedback?.first()?.screenshots?.map { it.id })

        val oldDesktop = AppStoreStatusLoader.load(
            { server },
            { Json.parseObject("""{"ok": true, "source": "desktop", "snapshot": $withoutScreenshots}""") },
            describe,
        )
        assertEq(false, oldDesktop.reply?.fromDesktop)
        assertEq(1760000300000L, oldDesktop.reply?.publishedAt)

        val failing = AppStoreStatusLoader.load({ server }, { throw AppStoreDesktopException("not_connected", "The desktop is not connected.") }, describe)
        assertEq(false, failing.reply?.fromDesktop)
        assertEq("", failing.error)
        expect(failing.reply?.snapshot?.lacksScreenshots == true)

        var asked = false
        val current = AppStoreStatusLoader.load({ reply(example) }, { asked = true; fresh }, describe)
        expect(!asked)
        assertEq(false, current.reply?.fromDesktop)
    }

    fun testFallsBackToTheDesktopWhenTheServerHasNothing() {
        val fresh = Json.parseObject("""{"ok": true, "source": "desktop", "snapshot": $example}""")
        val describe: (Throwable) -> String = { it.message.orEmpty() }
        val missing = AppStoreStatusLoader.load({ throw PackagesException("http_404", 404) }, { fresh }, describe)
        assertEq(true, missing.reply?.fromDesktop)
        val offline = AppStoreStatusLoader.load({ throw PackagesException("offline") }, { error("not asked") }, describe)
        assertEq(null, offline.reply)
        val both = AppStoreStatusLoader.load(
            { throw PackagesException("http_404", 404) },
            { throw AppStoreDesktopException("app_store_report_failed", "missing ASC_KEY_ID.") },
            { "Not offered." },
        )
        assertEq("Not offered. Your desktop could not fetch it either: missing ASC_KEY_ID.", both.error)
    }
}
