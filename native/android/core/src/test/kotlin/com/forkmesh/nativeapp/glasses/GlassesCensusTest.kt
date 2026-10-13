package com.forkmesh.nativeapp.glasses

import com.forkmesh.nativeapp.testing.assertEq
import java.util.UUID

class GlassesCensusTest {
    private fun device(address: String, name: String?, rssi: Int, at: Long, vararg ids: Int) =
        NearbyKinds.device(GlassesAdvert(address, name, rssi, ids.toList()), at)

    fun testSortsDevicesIntoKinds() {
        assertEq(NearbyKind.HEADPHONES, NearbyKinds.kind("Galaxy Buds2 Pro", emptyList()))
        assertEq(NearbyKind.HEADPHONES, NearbyKinds.kind("AirPods Pro", listOf(0x004C)))
        assertEq(NearbyKind.WATCH, NearbyKinds.kind("Pixel Watch 2", emptyList()))
        assertEq(NearbyKind.TV, NearbyKinds.kind("[TV] Samsung Q80", emptyList()))
        assertEq(NearbyKind.PHONE, NearbyKinds.kind(null, listOf(0x004C)))
        assertEq(NearbyKind.TAG, NearbyKinds.kind(null, listOf(0x00D2)))
        assertEq(NearbyKind.LIGHT, NearbyKinds.kind("Govee_H6008", emptyList()))
        assertEq(NearbyKind.LIGHT, NearbyKinds.kind("GBK_H6159", emptyList()))
        assertEq(NearbyKind.APPLIANCE, NearbyKinds.kind("Vertuo_DV4", emptyList()))
        assertEq(NearbyKind.TV, NearbyKinds.kind("tcl_B14S5", emptyList()))
        assertEq(NearbyKind.OTHER, NearbyKinds.kind("ZX-41", listOf(0x1234)))
        assertEq("Apple", NearbyKinds.maker(listOf(0x004C)))
    }

    fun testSortsDevicesByNameAppearanceServiceAndMaker() {
        assertEq(NearbyKind.GLASSES, NearbyKinds.kind("Echo Frames", emptyList()))
        assertEq(NearbyKind.HEARING_AID, NearbyKinds.kind("Phonak Audeo", emptyList()))
        assertEq(NearbyKind.GAMING, NearbyKinds.kind("Wireless Controller", emptyList()))
        assertEq(NearbyKind.CAMERA, NearbyKinds.kind("GoPro 1234", emptyList()))
        assertEq(NearbyKind.DRONE, NearbyKinds.kind("Mavic Air 2", emptyList()))
        assertEq(NearbyKind.FITNESS, NearbyKinds.kind("Polar H10 1A2B", emptyList()))
        assertEq(NearbyKind.LOCK, NearbyKinds.kind("Yale Lock", emptyList()))
        assertEq(NearbyKind.SMART_HOME, NearbyKinds.kind("SwitchBot Meter", emptyList()))
        assertEq(NearbyKind.RIDE, NearbyKinds.kind("Ninebot Max", emptyList()))
        assertEq(NearbyKind.TABLET, NearbyKinds.kind("Galaxy Tab S9", emptyList()))
        assertEq(NearbyKind.SPEAKER, NearbyKinds.kind("LE-Bose Revolve+", emptyList()))
        assertEq(NearbyKind.PRINTER, NearbyKinds.kind("Brother PT-P710", emptyList()))
        assertEq(NearbyKind.SCALE, NearbyKinds.kind("RENPHO", emptyList()))
        assertEq(NearbyKind.HEALTH, NearbyKinds.kind("Dexcom G7", emptyList()))
        assertEq(NearbyKind.APPLIANCE, NearbyKinds.kind("Sonicare", emptyList()))
        assertEq(NearbyKind.PHONE, NearbyKinds.kind("Sam's iPhone", emptyList()))
        assertEq(NearbyKind.HEADPHONES, NearbyKinds.kind("Bose QC45", emptyList()))
        assertEq(NearbyKind.WATCH, NearbyKinds.kind(null, emptyList(), appearance = 0x00C1))
        assertEq(NearbyKind.GAMING, NearbyKinds.kind(null, emptyList(), appearance = 0x03C4))
        assertEq(NearbyKind.HEADPHONES, NearbyKinds.kind(null, emptyList(), appearance = 0x0941))
        assertEq(NearbyKind.FITNESS, NearbyKinds.kind(null, listOf(0x004C), listOf(0x180D)))
        assertEq(NearbyKind.PHONE, NearbyKinds.kind(null, emptyList(), listOf(0xFD6F)))
        assertEq(NearbyKind.INPUT, NearbyKinds.kind(null, listOf(0x01DA)))
        assertEq(0x180D, NearbyKinds.shortUuid(UUID.fromString("0000180d-0000-1000-8000-00805f9b34fb")))
        assertEq(null, NearbyKinds.shortUuid(UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")))
        assertEq(0x00C1, NearbyKinds.appearance(byteArrayOf(0x02, 0x01, 0x06, 0x03, 0x19, 0xC1.toByte(), 0x00)))
        assertEq(null, NearbyKinds.appearance(byteArrayOf(0x02, 0x01, 0x06, 0x03, 0x19, 0xC1.toByte())))
        assertEq(null, NearbyKinds.appearance(null))
        val census = GlassesCensus()
        census.observe(NearbyDevice("A", null, null, NearbyKind.OTHER, -60, 0))
        census.observe(NearbyDevice("A", null, null, NearbyKind.FITNESS, -60, 1_000))
        assertEq(NearbyKind.FITNESS, census.devices(2_000).first().kind)
    }

    fun testLabelsFallBackToMakerThenKind() {
        assertEq("Kitchen speaker", device("A", " Kitchen speaker ", -60, 0)?.label)
        assertEq("Apple device", device("B", null, -60, 0, 0x004C)?.label)
        assertEq("Bluetooth device", device("C", "", -60, 0)?.label)
        assertEq(null, device("D", null, 127, 0))
    }

    fun testKeepsNamesForgetsOldDevicesAndSortsByStrength() {
        val census = GlassesCensus(forgetAfterMs = 10_000, limit = 2)
        census.observe(device("A", "Pixel 8", -70, 0)!!)
        census.observe(device("A", null, -65, 1_000)!!)
        census.observe(device("B", null, -50, 2_000, 0x004C)!!)
        census.observe(device("C", null, -90, 3_000)!!)
        val now = census.devices(4_000)
        assertEq(listOf("B", "A"), now.map { it.address })
        assertEq("Pixel 8", now[1].name)
        assertEq(NearbyKind.PHONE, now[1].kind)
        assertEq(-68, now[1].rssi)
        assertEq(listOf("B", "C"), census.devices(12_000).map { it.address })
        census.reset()
        assertEq(0, census.devices(12_000).size)
    }

    fun testKeepsDevicesForFortyFiveSecondsByDefault() {
        val census = GlassesCensus()
        census.observe(device("A", "Pixel 8", -70, 0)!!)
        assertEq(listOf("A"), census.devices(45_000).map { it.address })
        assertEq(0, census.devices(45_001).size)
    }

    private fun rounded(value: Double) = Math.round(value * 1000) / 1000.0

    fun testRadarPlacesDevicesByStrengthAndGlassesByDirection() {
        assertEq(81.5, GlassesRadarLayout.angle("AA:BB:CC:DD:EE:FF"))
        assertEq(0.2, GlassesRadarLayout.distance(-40))
        assertEq(0.5, GlassesRadarLayout.distance(-70))
        assertEq(0.92, GlassesRadarLayout.distance(-100))
        val others = listOf(device("A", "Pixel 8", -70, 0)!!)
        val sighting = GlassesSighting("G", "Ray-Ban", null, true, -80, 0)
        val homing = GlassesHomingReading("G", "Ray-Ban", -60, 0.7, null, 100.0, 1.0, true, true)
        val blips = GlassesRadarLayout.blips(others, listOf(sighting), homing, 40.0)
        assertEq(listOf("A", "G"), blips.map { it.address })
        assertEq(GlassesRadarLayout.angle("A"), blips[0].angle)
        assertEq(false, blips[0].glasses)
        assertEq(true, blips[1].glasses)
        assertEq(60.0, rounded(blips[1].angle))
        assertEq(0.3, rounded(blips[1].distance))
        assertEq(GlassesRadarLayout.angle("G"), GlassesRadarLayout.blips(emptyList(), listOf(sighting), homing, null)[0].angle)
    }
}
