package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.CryptoInterop
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceBytes
import com.forkmesh.nativeapp.testing.resourceText
import java.io.File

@Suppress("UNCHECKED_CAST")
class TrustListsCryptoTest {
    private val vectors = Json.parse(resourceText("trust-lists/v1/vectors.json")) as Map<String, Any?>
    private val cases = vectors["cases"] as List<Map<String, Any?>>
    private val testKeys = vectors["testKeys"] as Map<String, String>

    private fun resolveCase(vector: Map<String, Any?>, reversed: Boolean = false): Map<String, Any?> {
        val input = vector["input"] as Map<String, Any?>
        fun <T> ordered(values: List<T>): List<T> = if (reversed) values.reversed() else values
        return TrustLists.resolve(
            ordered(input["documents"] as List<String>),
            now = input["now"] as Long,
            follows = ordered(input["follows"] as List<Map<String, Any?>>),
            overrides = ordered(input["overrides"] as List<Map<String, Any?>>),
        )
    }

    private fun caseNamed(id: String): Map<String, Any?> = cases.first { it["id"] == id }

    private fun sortedJson(value: Any?): String = when (value) {
        is Map<*, *> -> value.keys.map { it as String }.sorted().joinToString(",", "{", "}") { Json.stringify(it) + ":" + sortedJson(value[it]) }
        is List<*> -> value.joinToString(",", "[", "]") { sortedJson(it) }
        else -> Json.stringify(value)
    }

    private fun checkAll(reversed: Boolean) {
        val failures = ArrayList<String>()
        for (vector in cases) {
            val id = vector["id"] as String
            val got = resolveCase(vector, reversed)
            if (got != vector["expect"]) {
                val a = sortedJson(vector["expect"])
                val b = sortedJson(got)
                val at = a.indices.firstOrNull { it >= b.length || a[it] != b[it] } ?: a.length
                failures.add("$id: expected …${a.substring(maxOf(0, at - 80), minOf(a.length, at + 80))}… got …${b.substring(maxOf(0, at - 80), minOf(b.length, at + 80))}…")
            }
        }
        expect(failures.isEmpty(), failures.joinToString("\n"))
        println("       ${cases.size} vectors match${if (reversed) " in reversed input order" else ""}")
    }

    fun testVectorsArePresent() {
        expect(cases.isNotEmpty())
        assertEq("forkmesh-trust-list-vectors-v1", vectors["schema"])
        assertEq(24, cases.size)
    }

    fun testEveryCaseMatchesTheSharedResolution() = checkAll(reversed = false)

    fun testEveryCaseDoesNotDependOnInputOrder() = checkAll(reversed = true)

    fun testCompromiseDrillFlipsTheListFromActiveToRecovered() {
        val alice = testKeys.getValue("alice")
        val malicious = setOf(testKeys.getValue("mallory"), testKeys.getValue("maintainer"))

        fun aliceRow(resolution: Map<String, Any?>): Map<String, Any?> =
            (resolution["lists"] as List<Map<String, Any?>>).first { it["issuer"] == alice && it["name"] == "security" }
        fun subjects(resolution: Map<String, Any?>): Set<Any?> =
            (resolution["assertions"] as List<Map<String, Any?>>).map { it["subject"] }.toSet()

        val before = resolveCase(caseNamed("compromise-drill-before-revocation"))
        assertEq("active", aliceRow(before)["state"])
        assertEq(3L, aliceRow(before)["headVersion"])
        expect(subjects(before).containsAll(malicious))

        val after = resolveCase(caseNamed("compromise-drill-revoked-and-recovered"))
        val row = aliceRow(after)
        assertEq("recovered", row["state"])
        assertEq(2L, row["headVersion"])
        assertEq(3L, row["cutoffVersion"])
        expect((row["cutoffReasons"] as List<*>).contains("revoked"))
        assertEq(mapOf("issuer" to testKeys["alice-successor"], "name" to "security"), row["successor"])
        expect(subjects(after).intersect(malicious).isEmpty())
        assertEq(1L, (after["telemetry"] as Map<String, Any?>)["revocations"])
    }

    fun testVerifyDocumentReportsStableCodes() {
        fun codeOf(raw: String): String? = try {
            TrustLists.verifyDocument(raw)
            null
        } catch (error: TrustListError) {
            error.code
        }

        assertEq("invalid_json", codeOf("{"))
        assertEq("unsupported_schema", codeOf("[]"))
        assertEq("noncanonical", codeOf("""{"a":1.5}"""))
        assertEq("noncanonical", codeOf("""{"a":9007199254740992}"""))
        assertEq("noncanonical", codeOf("""{ "a":1}"""))
        assertEq("invalid_keys", codeOf("""{"schema":"forkmesh-trust-list-v1"}"""))
        assertEq("""{"a":null,"b":1}""", TrustLists.canonicalJson(linkedMapOf("b" to 1L, "a" to null)))
        assertEq("sha256:45e334b6c74ca5db8d8f8fcd1157fb31a2ebc9953e0d8d182e37b6b67e6a1705", TrustLists.documentId("a\uD800b"))
        assertEq("sha256:d506f8eb09b320da26e02670fb6ce1685d67541cb373a98c00adbffadef794f4", TrustLists.documentId("x😀y"))
        assertEq("invalid_json", codeOf("a\uD800b"))
        for (raw in listOf("""{"a":01}""", """{"a":1.}""", """{"a":-}""", """{"a":"\x"}""", """{"a":tru}""", "{\"a\":\"\u0001\"}")) {
            assertEq("invalid_json", codeOf(raw), raw)
        }
        assertEq("invalid_json", codeOf("[".repeat(9) + "]".repeat(9)))
        assertEq("unsupported_schema", codeOf("[".repeat(8) + "]".repeat(8)))
        assertEq("too_large", codeOf("\"" + "a".repeat(262144) + "\""))
    }

    fun testBundledVectorsMatchTheRepositoryCopy() {
        val repository = CryptoInterop.repoRoot?.let { File(it, "conformance/trust-lists/v1/vectors.json") }
        if (repository == null || !repository.isFile) {
            println("       skip: conformance/trust-lists/v1/vectors.json not reachable from this tree")
            return
        }
        expect(repository.readBytes().contentEquals(resourceBytes("trust-lists/v1/vectors.json")), "core/src/test/resources/trust-lists/v1/vectors.json is stale; copy it from $repository")
    }
}
