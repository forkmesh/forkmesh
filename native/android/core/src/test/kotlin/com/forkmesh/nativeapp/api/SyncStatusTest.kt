package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class SyncStatusTest {
    private val fixture = Json.parseObject(resourceText("sync-status/fixture.json"))
    private val privateStrings = fixture.strings("privateStrings")

    private fun expectCase(id: String, got: Map<String, Any?>, want: Any?) {
        assertEq(want, got, id)
        val text = Json.stringify(got)
        for (secret in privateStrings) expect(!text.contains(secret), "$id leaks $secret")
    }

    fun testEvidenceCasesMatchTheSharedFixture() {
        val cases = fixture.maps("cases")
        expect(cases.isNotEmpty())
        for (entry in cases) {
            expectCase(entry.str("id"), evaluateSyncStatus(entry.obj("evidence")), entry["expect"])
        }
    }

    fun testTransitionSequencesNeverReportFalseSuccess() {
        val sequences = fixture.maps("sequences")
        expect(sequences.isNotEmpty())
        for (sequence in sequences) {
            val steps = sequence.maps("steps")
            for ((i, step) in steps.withIndex()) {
                expectCase("${sequence.str("id")}#$i", evaluateSyncStatus(step.obj("evidence")), step["expect"])
            }
        }
    }

    fun testMirrorsPayloadsMatchTheSharedFixture() {
        val cases = fixture.maps("mirrorsPayloadCases")
        expect(cases.isNotEmpty())
        for (entry in cases) {
            expectCase(entry.str("id"), mirrorsPayloadSyncStatus(entry.obj("payload")), entry["expect"])
        }
    }

    fun testOutputKeepsTheDartKeyOrder() {
        val result = evaluateSyncStatus(mapOf("now" to 1L))
        assertEq(
            listOf(
                "schema", "state", "label", "severity", "current", "totalCopies", "nodesReached",
                "verifiedCopies", "trustedMirrors", "lastConfirmedAt", "lagMs", "error", "action", "summary",
            ),
            result.keys.toList(),
        )
        assertEq("stale", result["state"])
        assertEq("unconfirmed", result["error"])
    }
}
