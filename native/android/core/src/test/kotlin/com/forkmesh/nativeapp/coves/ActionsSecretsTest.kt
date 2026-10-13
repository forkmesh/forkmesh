package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

private class FakeSecretsChannel : ControlCalling {
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    val answers = HashMap<String, Map<String, Any?>>()

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls.add(method to params)
        return answers[method] ?: emptyMap()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = Any()

    override fun removeEventTap(token: Any) {}
}

class ActionsSecretsTest {
    fun testNameValidation() {
        expect(ActionsSecrets.validName("CLOUDFLARE_API_TOKEN"))
        expect(ActionsSecrets.validName("_x9"))
        expect(!ActionsSecrets.validName("9x"))
        expect(!ActionsSecrets.validName("A-B"))
        expect(!ActionsSecrets.validName(""))
        expect(!ActionsSecrets.validName("A".repeat(129)))
        expect(!ActionsSecrets.validName("ÄB"))
    }

    fun testRowsNeverCarryValuesAndAreBounded() {
        val answer = mapOf(
            "variables" to listOf(
                mapOf("name" to "b_token", "value" to "leak"),
                mapOf("name" to "A", "updatedAtMs" to 5L),
                mapOf("name" to "bad name"),
                mapOf("name" to "A"),
            ),
        )
        assertEq(listOf(ActionsSecretRow("A", 5), ActionsSecretRow("b_token")), ActionsSecrets.rows(answer))
    }

    fun testSaveAsksForAConfirmationThenWrites() {
        val channel = FakeSecretsChannel()
        channel.answers["secrets.prepare"] = mapOf("token" to "t1", "summary" to "Set TOKEN")
        channel.answers["secrets.list"] = mapOf("variables" to listOf(mapOf("name" to "TOKEN")))
        val model = ActionsSecretsModel(ActionsSecretsClient(channel))
        expect(model.save(" TOKEN ", "s3cret"))
        assertEq(listOf("secrets.prepare", "secrets.set", "secrets.list"), channel.calls.map { it.first })
        assertEq("set", channel.calls[0].second["action"])
        expect(!channel.calls[0].second.containsKey("value"))
        assertEq("t1", channel.calls[1].second["token"])
        assertEq("s3cret", channel.calls[1].second["value"])
        assertEq(listOf("TOKEN"), model.rows.map { it.name })
        expect(model.delete("TOKEN"))
        assertEq("delete", channel.calls[3].second["action"])
        assertEq("secrets.delete", channel.calls[4].first)
    }

    fun testRefusalsAreReported() {
        val channel = FakeSecretsChannel()
        channel.answers["secrets.prepare"] = mapOf("token" to "t")
        channel.answers["secrets.set"] = mapOf("error" to "confirmation_expired")
        val model = ActionsSecretsModel(ActionsSecretsClient(channel))
        expect(!model.save("OK", "v"))
        assertEq("The confirmation expired. Try again.", model.error)
        expect(!model.save("no good", "v"))
        assertEq(2, channel.calls.size)
        expect(!model.save("BIG", "x".repeat(16 * 1024 + 1)))
        assertEq("Values are limited to 16 KB.", model.error)
        assertEq(2, channel.calls.size)
    }
}
