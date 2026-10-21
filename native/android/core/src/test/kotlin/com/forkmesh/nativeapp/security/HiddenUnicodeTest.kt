package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.CryptoInterop
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceBytes
import com.forkmesh.nativeapp.testing.resourceText
import java.io.File

@Suppress("UNCHECKED_CAST")
class HiddenUnicodeTest {
    private val vectors = Json.parse(resourceText("hidden-unicode/v1/vectors.json")) as Map<String, Any?>
    private val cases = vectors["cases"] as List<Map<String, Any?>>

    fun testPolicyName() {
        assertEq(HiddenUnicode.POLICY, vectors["policy"])
        expect(cases.size >= 40, "vectors are present")
    }

    fun testEveryVector() {
        for (vector in cases) {
            val id = vector["id"] as String
            val input = vector["input"] as String
            val output = vector["output"] as String
            val cleaned = HiddenUnicode.clean(input)
            assertEq(output, cleaned, id)
            assertEq(vector["hidden"] as Boolean, HiddenUnicode.hasHidden(input), id)
            assertEq(cleaned, HiddenUnicode.clean(cleaned), id)
        }
    }

    fun testLoneSurrogatesAreDropped() {
        assertEq("abc", HiddenUnicode.clean("a" + 0xD800.toChar() + "b" + 0xDFFF.toChar() + "c"))
    }

    fun testCleanedOffsetFollowsRemovals() {
        val text = "ab" + 0x200B.toChar() + 0x200B.toChar() + "cd"
        assertEq(3, HiddenUnicode.cleanedOffset(text, 5))
        assertEq(4, HiddenUnicode.cleanedOffset(text, 99))
    }

    fun testBundledVectorsMatchTheRepositoryCopy() {
        val repository = CryptoInterop.repoRoot?.let { File(it, "conformance/hidden-unicode/v1/vectors.json") }
        if (repository == null || !repository.isFile) {
            println("       skip: conformance/hidden-unicode/v1/vectors.json not reachable from this tree")
            return
        }
        expect(
            repository.readBytes().contentEquals(resourceBytes("hidden-unicode/v1/vectors.json")),
            "core/src/test/resources/hidden-unicode/v1/vectors.json is stale; copy it from $repository",
        )
    }
}
