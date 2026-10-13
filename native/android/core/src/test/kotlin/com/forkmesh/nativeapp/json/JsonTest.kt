package com.forkmesh.nativeapp.json

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class JsonTest {
    fun testParsesNestedValues() {
        val map = Json.parseObject(
            """{"a":1,"b":[true,null,"x"],"c":{"d":-2.5},"e":"é\n"}""",
        )
        assertEq(1L, map["a"])
        assertEq(listOf(true, null, "x"), map["b"])
        assertEq(-2.5, map.obj("c")["d"])
        assertEq("é\n", map["e"])
    }

    fun testWritesLikeDartJsonEncode() {
        val map = jsonOf(
            "name" to "fork\"mesh", "n" to 3L, "f" to 1.0, "ok" to true,
            "list" to listOf(1L, "two"), "none" to null, "uni" to "é",
        )
        assertEq(
            """{"name":"fork\"mesh","n":3,"f":1.0,"ok":true,"list":[1,"two"],"none":null,"uni":"é"}""",
            Json.stringify(map),
        )
    }

    fun testRoundTripsLargeIntegers() {
        assertEq(1786636663064L, Json.parse("1786636663064"))
        assertEq("1786636663064", Json.stringify(Json.parse("1786636663064")))
    }

    fun testRejectsInvalidInput() {
        assertFails<Json.JsonException> { Json.parse("""{"a":}""") }
        assertFails<Json.JsonException> { Json.parse("[1,2") }
        assertFails<Json.JsonException> { Json.parse("{} x") }
        assertEq(0, Json.parseObject("not json").size)
    }

    fun testLenientAccessors() {
        val map = Json.parseObject("""{"s":5,"n":"42","b":"true","l":[{"x":1},2],"d":2.0}""")
        assertEq("5", map.str("s"))
        assertEq("", map.str("missing"))
        assertEq(42L, map.long("n"))
        expect(!map.bool("b"), "only a real true is true")
        assertEq(1, map.maps("l").size)
        assertEq("2.0", map.str("d"))
        assertEq("5", map.firstStr("missing", "s"))
    }
}
