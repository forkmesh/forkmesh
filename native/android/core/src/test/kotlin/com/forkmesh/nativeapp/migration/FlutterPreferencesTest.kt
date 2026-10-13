package com.forkmesh.nativeapp.migration

import com.forkmesh.nativeapp.testing.assertEq
import java.io.ByteArrayOutputStream
import java.io.ObjectOutputStream

class FlutterPreferencesTest {
    private fun platformEncodedList(list: List<String>): String {
        val bytes = ByteArrayOutputStream()
        ObjectOutputStream(bytes).use { it.writeObject(ArrayList(list)) }
        return FlutterPreferences.LIST_PREFIX + Base64s.encodeDefault(bytes.toByteArray())
    }

    fun testEveryLegacyPluginEncoding() {
        val unreadable = ArrayList<String>()
        val decoded = FlutterPreferences.decode(
            linkedMapOf(
                "flutter.relays/active" to "app.next.forkmesh.com",
                "flutter.node/serve" to true,
                "flutter.ads/hiddenUntilMs" to 1_790_000_000_000L,
                "flutter.bugReport/anchor" to FlutterPreferences.DOUBLE_PREFIX + "0.35",
                "flutter.recent" to FlutterPreferences.JSON_LIST_PREFIX + "[\"a\",\"b \\\"q\\\"\",\"é\"]",
                "flutter.oldList" to platformEncodedList(listOf("x", "y")),
                "flutter.big" to FlutterPreferences.BIG_INTEGER_PREFIX + "zz",
                "flutter.stringSet" to linkedSetOf("s1"),
                "flutter.broken" to FlutterPreferences.DOUBLE_PREFIX + "not-a-number",
                "notFlutter" to "ignored",
                "flutter.float" to 1.5f,
            ),
            unreadable,
        )
        assertEq(PreferenceValue.Text("app.next.forkmesh.com"), decoded["relays/active"])
        assertEq(PreferenceValue.Bool(true), decoded["node/serve"])
        assertEq(PreferenceValue.Int64(1_790_000_000_000L), decoded["ads/hiddenUntilMs"])
        assertEq(PreferenceValue.Real(0.35), decoded["bugReport/anchor"])
        assertEq(PreferenceValue.TextList(listOf("a", "b \"q\"", "é")), decoded["recent"])
        assertEq(PreferenceValue.TextList(listOf("x", "y")), decoded["oldList"])
        assertEq(PreferenceValue.Int64(35L * 36 + 35), decoded["big"])
        assertEq(PreferenceValue.TextList(listOf("s1")), decoded["stringSet"])
        assertEq(PreferenceValue.Real(1.5), decoded["float"])
        assertEq(null, decoded["notFlutter"])
        assertEq(listOf("broken"), unreadable)
    }

    fun testSerializedListRefusesOtherClasses() {
        val bytes = ByteArrayOutputStream()
        ObjectOutputStream(bytes).use { it.writeObject(java.util.HashMap<String, String>()) }
        val raw = FlutterPreferences.LIST_PREFIX + Base64s.encodeDefault(bytes.toByteArray())
        val unreadable = ArrayList<String>()
        val decoded = FlutterPreferences.decode(mapOf("flutter.evil" to raw), unreadable)
        assertEq(0, decoded.size)
        assertEq(listOf("evil"), unreadable)
    }
}
