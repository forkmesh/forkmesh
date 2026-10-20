package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MessageHandoffTest {
    fun testCleansAndTruncates() {
        assertEq("Could not reach ForkMesh.", MessageHandoff.cleaned("  Could not reach ForkMesh.\n"))
        val long = "x".repeat(MessageHandoff.MAX_MESSAGE_LENGTH + 10)
        assertEq(MessageHandoff.MAX_MESSAGE_LENGTH + 1, MessageHandoff.cleaned(long).length)
        expect(MessageHandoff.cleaned(long).endsWith("…"), "ellipsis")
        expect(!MessageHandoff.canHandOff("   \n"), "blank messages cannot be handed off")
    }

    fun testQuotesEveryLineForTheAgent() {
        assertEq(
            "The ForkMesh Android app showed this message:\n\n> Sync failed\n> \n> 401\n\nFind out what caused it and fix it.",
            MessageHandoff.agentPrompt("Sync failed\n\n401"),
        )
        assertEq(MessageHandoff.agentPrompt("Hi"), MessageHandoff.composeTarget("Hi").text)
    }
}
