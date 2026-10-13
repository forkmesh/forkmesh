package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.model.SponsoredAd
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException

class SponsoredAdTest {
    private val id = "0123456789abcdef0123456789abcdef"

    private fun payload(vararg overrides: Pair<String, Any?>): LinkedHashMap<String, Any?> =
        linkedMapOf<String, Any?>(
            "id" to id,
            "label" to "Sponsored",
            "headline" to "Ship signed releases",
            "body" to "Reproducible builds for small teams.",
            "cta" to "See how",
            "sponsor" to "Example Tools",
            "url" to "https://example.com/releases",
            "category" to "developer-tools",
            "categoryLabel" to "Developer tools and libraries",
        ).apply { putAll(overrides) }

    private fun api(transport: FakeTransport, token: String = "", clock: () -> Long = { System.currentTimeMillis() }): ApiService =
        ApiService(TestSettings(), transport, clock).also { if (token.isNotEmpty()) it.sessionTokenProvider = { token } }

    fun testAcceptsAWellFormedAd() {
        val ad = SponsoredAd.fromJson(payload())!!
        assertEq(id, ad.id)
        assertEq("Ship signed releases", ad.headline)
        assertEq("Reproducible builds for small teams.", ad.body)
        assertEq("See how", ad.cta)
        assertEq("Example Tools", ad.sponsor)
        assertEq("https://example.com/releases", ad.url)
        assertEq("example.com", ad.host)
        assertEq("Developer tools and libraries", ad.categoryLabel)
    }

    fun testRejectsNullNonMapsAndAMissingAd() {
        assertEq(null, SponsoredAd.fromJson(null))
        assertEq(null, SponsoredAd.fromJson("ad"))
        assertEq(null, SponsoredAd.fromJson(emptyList<Any?>()))
        assertEq(null, SponsoredAd.fromJson(emptyMap<String, Any?>()))
    }

    fun testRejectsEveryDestinationThatIsNotPlainHttps() {
        val urls = listOf<Any?>(
            "http://example.com/",
            "javascript:alert(1)",
            "JAVASCRIPT:alert(1)",
            "data:text/html,<script>alert(1)</script>",
            "intent://example.com#Intent;scheme=https;end",
            "forkmesh://open",
            "content://com.forkmesh.nativeapp/secret",
            "file:///etc/passwd",
            "//example.com/",
            "example.com",
            "https://",
            "https:///path",
            "https://forkmesh.com@evil.example/",
            "https://example.com\\@evil.example/",
            "HTTPS://example.com/",
            "https://example.com/ a",
            "https://example.com/\njavascript:alert(1)",
            "https://" + "a".repeat(SponsoredAd.MAX_URL_LENGTH) + ".example/",
            "",
            42L,
            null,
        )
        for (url in urls) {
            assertEq(null, SponsoredAd.fromJson(payload("url" to url)), "url: $url")
        }
    }

    fun testRejectsIdsThatAreNot32LowercaseHexCharacters() {
        val ids = listOf<Any?>(
            "",
            "abc",
            "0123456789abcdef0123456789abcde",
            "0123456789abcdef0123456789abcdef0",
            "0123456789ABCDEF0123456789ABCDEF",
            "0123456789abcdef0123456789abcdeg",
            "../../accounts/0123456789abcdef01",
            123L,
            null,
        )
        for (bad in ids) {
            assertEq(null, SponsoredAd.fromJson(payload("id" to bad)), "id: $bad")
        }
    }

    fun testRejectsAnEmptyOrNonTextHeadline() {
        assertEq(null, SponsoredAd.fromJson(payload("headline" to "")))
        assertEq(null, SponsoredAd.fromJson(payload("headline" to "   \n ")))
        assertEq(null, SponsoredAd.fromJson(payload("headline" to 7L)))
        assertEq(null, SponsoredAd.fromJson(payload().apply { remove("headline") }))
    }

    fun testClampsEveryTextFieldToThePublishedLimits() {
        val ad = SponsoredAd.fromJson(
            payload(
                "headline" to "h".repeat(200),
                "body" to "b".repeat(500),
                "cta" to "c".repeat(80),
                "sponsor" to "s".repeat(90),
            ),
        )!!
        assertEq(60, ad.headline.length)
        assertEq(160, ad.body.length)
        assertEq(24, ad.cta.length)
        assertEq(48, ad.sponsor.length)
    }

    fun testClampingNeverSplitsASurrogatePair() {
        val rocket = String(Character.toChars(0x1F680))
        val ad = SponsoredAd.fromJson(payload("headline" to rocket.repeat(80)))!!
        assertEq(60, ad.headline.codePointCount(0, ad.headline.length))
        assertEq(rocket.repeat(60), ad.headline)
    }

    fun testCollapsesWhitespaceAndFillsOptionalFields() {
        val ad = SponsoredAd.fromJson(
            payload(
                "headline" to "  Two\n\nlines\tof   text ",
                "body" to null,
                "cta" to "",
                "sponsor" to 12L,
            ).apply { remove("categoryLabel") },
        )!!
        assertEq("Two lines of text", ad.headline)
        assertEq("", ad.body)
        assertEq(SponsoredAd.DEFAULT_CTA, ad.cta)
        assertEq("", ad.sponsor)
        assertEq("", ad.categoryLabel)
    }

    fun testSponsoredMessagesDefaultOnAndTheSwitchPersists() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        expect(settings.showSponsoredMessages)
        settings.setShowSponsoredMessages(false)
        expect(!AccountSettings.create(prefs, MemorySecretStore()).showSponsoredMessages)
    }

    fun testHideLastsSevenDaysAndNotifies() {
        val prefs = MemoryPrefStore()
        val settings = AccountSettings.create(prefs, MemorySecretStore())
        var notified = 0
        settings.addListener { notified++ }
        val now = 1_700_000_000_000L
        expect(!settings.sponsoredMessagesHidden(now))
        settings.hideSponsoredMessages(now)
        assertEq(1, notified)
        val week = 7L * 24 * 60 * 60 * 1000
        val reloaded = AccountSettings.create(prefs, MemorySecretStore())
        assertEq(now + week, reloaded.sponsoredHiddenUntilMs)
        expect(reloaded.sponsoredMessagesHidden(now + week - 1))
        expect(!reloaded.sponsoredMessagesHidden(now + week))
        prefs.putString(AccountSettings.SPONSORED_HIDDEN_UNTIL_KEY, "not a number")
        expect(!reloaded.sponsoredMessagesHidden(now))
    }

    fun testServeIsUnauthenticatedForTheMobilePlacementAndCached() {
        val transport = FakeTransport { jsonResponse(mapOf("ok" to true, "placement" to "mobile", "ad" to payload())) }
        val api = api(transport, token = "secret-session-token")
        assertEq(id, api.sponsoredAd()?.id)
        assertEq(id, api.sponsoredAd()?.id)
        assertEq(1, transport.requests.size, "second call is served from cache")
        val request = transport.requests.single()
        assertEq("GET", request.method)
        assertEq("/api/ads/serve", request.path())
        assertEq(mapOf("placement" to "mobile"), request.query())
        assertEq(null, request.headerValue("Authorization"))
        assertEq(null, request.headerValue("Cookie"))
    }

    fun testANullAdAnInvalidAdAndAFailureAreAllNullOnce() {
        val responses: Map<String, () -> HttpClient.Response> = linkedMapOf(
            "null ad" to { jsonResponse(mapOf("ok" to true, "ad" to null)) },
            "invalid ad" to { jsonResponse(mapOf("ok" to true, "ad" to payload("url" to "http://example.com/"))) },
            "not ok" to { jsonResponse(mapOf("ok" to false, "ad" to payload())) },
            "server error" to { textResponse("boom", 500) },
            "not json" to { textResponse("<html>") },
            "offline" to { throw IOException("offline") },
        )
        for ((name, respond) in responses) {
            val transport = FakeTransport { respond() }
            val api = api(transport)
            assertEq(null, api.sponsoredAd(), name)
            assertEq(null, api.sponsoredAd(), name)
            assertEq(1, transport.requests.size, "$name: never retried")
        }
    }

    fun testCacheExpiresAfterThirtyMinutesAndFailuresAfterFive() {
        var now = 1_000_000L
        var fail = false
        val transport = FakeTransport {
            if (fail) textResponse("boom", 500) else jsonResponse(mapOf("ok" to true, "ad" to payload()))
        }
        val api = api(transport, clock = { now })
        expect(api.sponsoredAd() != null)
        now += ApiService.SPONSORED_AD_TTL_MS - 1
        api.sponsoredAd()
        assertEq(1, transport.requests.size)
        now += 1
        fail = true
        assertEq(null, api.sponsoredAd())
        assertEq(2, transport.requests.size)
        now += ApiService.SPONSORED_AD_FAILURE_TTL_MS - 1
        assertEq(null, api.sponsoredAd())
        assertEq(2, transport.requests.size)
        now += 1
        fail = false
        expect(api.sponsoredAd() != null)
        assertEq(3, transport.requests.size)
    }

    fun testClickIsAnUnauthenticatedPostAndSwallowsFailures() {
        val transport = FakeTransport { throw IOException("offline") }
        val api = api(transport, token = "secret-session-token")
        api.sponsoredAdClicked(id)
        api.sponsoredAdClicked("../../accounts/me")
        assertEq(1, transport.requests.size, "a malformed id is never sent")
        val request = transport.requests.single()
        assertEq("POST", request.method)
        assertEq("/api/ads/$id/click", request.path())
        assertEq(mapOf<String, Any?>("placement" to "mobile"), request.bodyJson())
        assertEq(null, request.headerValue("Authorization"))
    }
}
