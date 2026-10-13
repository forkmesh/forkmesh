package com.forkmesh.nativeapp.glasses

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class GlassesWatchTest {
    private fun advert(address: String = "AA", name: String? = null, rssi: Int = -60, vararg ids: Int) =
        GlassesAdvert(address, name, rssi, ids.toList())

    fun testReadsTheLittleEndianCompanyId() {
        assertEq(0x0D53, GlassesSignatures.companyId(byteArrayOf(0x53, 0x0D, 0x01)))
        assertEq(0x01AB, GlassesSignatures.companyId(byteArrayOf(0xAB.toByte(), 0x01)))
        assertEq(null, GlassesSignatures.companyId(byteArrayOf(0x53)))
    }

    fun testMatchesEveryMetaCompanyId() {
        for (maker in GlassesMaker.entries) {
            val sighting = GlassesSignatures.match(advert(ids = intArrayOf(maker.companyId)), GlassesRange.ROOM, 5)
            assertEq(maker, sighting?.maker)
        }
        assertEq(setOf(0x01AB, 0x058E, 0x0D53), GlassesSignatures.COMPANY_IDS)
    }

    fun testIgnoresOtherMakers() {
        assertEq(null, GlassesSignatures.match(advert(name = "Pixel Buds", ids = intArrayOf(0x00E0)), GlassesRange.WIDE, 0))
        assertEq(null, GlassesSignatures.match(advert(ids = intArrayOf(0x004C)), GlassesRange.WIDE, 0))
        assertEq(null, GlassesSignatures.match(advert(rssi = 127, ids = intArrayOf(0x0D53)), GlassesRange.WIDE, 0))
    }

    fun testMatchesGlassesNamesWithoutACompanyId() {
        val sighting = GlassesSignatures.match(advert(name = " Ray-Ban Meta 0F2A ", ids = intArrayOf()), GlassesRange.ROOM, 0)
        assertEq("Ray-Ban Meta 0F2A", sighting?.label)
        expect(sighting?.namedAsGlasses == true, "a glasses name is flagged")
        expect(GlassesSignatures.nameMatches("RAYBAN STORIES"), "names match case-insensitively")
        expect(!GlassesSignatures.nameMatches(""), "an empty name never matches")
    }

    fun testLabelsLuxotticaAsRayBanOrOakley() {
        assertEq("Ray-Ban or Oakley Meta glasses", GlassesSignatures.match(advert(ids = intArrayOf(0x0D53)), GlassesRange.ROOM, 0)?.label)
        assertEq("Meta glasses or headset", GlassesSignatures.match(advert(ids = intArrayOf(0x058E)), GlassesRange.ROOM, 0)?.label)
    }

    fun testRangeDropsWeakSignals() {
        val far = advert(rssi = -80, ids = intArrayOf(0x0D53))
        assertEq(null, GlassesSignatures.match(far, GlassesRange.ROOM, 0))
        expect(GlassesSignatures.match(far, GlassesRange.WIDE, 0) != null, "the wide range keeps a -80 dBm signal")
        assertEq(GlassesRange.ROOM, GlassesRange.of("bogus"))
        assertEq(GlassesRange.NEAR, GlassesRange.of("near"))
    }

    fun testDistanceWords() {
        assertEq("Very close", GlassesSignatures.distance(-55))
        assertEq("Close", GlassesSignatures.distance(-68))
        assertEq("In the area", GlassesSignatures.distance(-85))
    }

    fun testTrackerAlertsOncePerDeviceAndHonoursTheCooldown() {
        val tracker = GlassesTracker(forgetAfterMs = 60_000, cooldownMs = 20_000)
        fun seen(address: String, at: Long) = GlassesSighting(address, "x", GlassesMaker.META, false, -60, at)
        expect(tracker.observe(seen("A", 0)), "a new device alerts")
        expect(!tracker.observe(seen("A", 1_000)), "the same device does not alert again")
        expect(!tracker.observe(seen("B", 5_000)), "a second device inside the cooldown stays quiet")
        expect(tracker.observe(seen("C", 25_000)), "a new device after the cooldown alerts")
        assertEq(listOf("A", "B", "C"), tracker.nearby(30_000).map { it.address }.sorted())
        assertEq(listOf("C"), tracker.nearby(80_000).map { it.address })
        expect(tracker.observe(seen("A", 100_000)), "a forgotten device that comes back alerts again")
        tracker.reset()
        assertEq(0, tracker.nearby(100_000).size)
    }

    fun testNearbyListsStrongestFirst() {
        val tracker = GlassesTracker()
        tracker.observe(GlassesSighting("A", "x", null, true, -80, 0))
        tracker.observe(GlassesSighting("B", "x", null, true, -50, 0))
        assertEq(listOf("B", "A"), tracker.nearby(1).map { it.address })
    }
}
