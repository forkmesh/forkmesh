package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CrmZohoStatusTest {
    fun testZohoStatusCarriesSendingAndMissingSteps() {
        val readOnly = CrmZohoStatus.fromJson(
            mapOf(
                "connected" to true, "canSend" to false, "lastSyncAt" to 5L, "lastSentAt" to 0L,
                "steps" to listOf(mapOf("key" to "send", "action" to "reconnect", "text" to "Reconnect Zoho Mail."), mapOf("key" to "empty")),
            ),
        )
        expect(!readOnly.canSend)
        assertEq(listOf("reconnect"), readOnly.steps.map { it.action })
        val olderServer = CrmZohoStatus.fromJson(mapOf("connected" to true))
        expect(olderServer.canSend)
        expect(olderServer.steps.isEmpty())
        expect(!CrmZohoStatus.fromJson(null).canSend)
    }

    fun testSettingsReadTheZohoStatus() {
        val settings = CrmSettings.fromJson(mapOf("zoho" to mapOf("connected" to true, "region" to "eu", "canSend" to false, "lastSyncError" to "token_rejected")))
        assertEq("eu", settings.zoho.region)
        expect(!settings.zoho.canSend)
        assertEq("token_rejected", settings.zoho.lastSyncError)
    }

    fun testZohoReconnectRetainsClientIdAndUsesSavedSecretOnlyForThatClient() {
        val status = CrmZohoStatus.fromJson(mapOf("connected" to true, "region" to "eu", "clientId" to "saved-id", "hasClientSecret" to true))
        var code = CrmZohoCode.of(status)
        assertEq("saved-id", code.clientId)
        assertEq("eu", code.region)
        expect(code.clientSecret.isEmpty())
        expect(code.usesSavedSecret)
        expect(!code.isComplete)
        code = code.copy(code = "fresh-code")
        expect(code.isComplete)
        assertEq("", code.body["clientSecret"])
        code = code.copy(clientId = "different-id")
        expect(!code.usesSavedSecret)
        expect(!code.isComplete)
        code = code.copy(clientSecret = "new-secret")
        expect(code.isComplete)
        expect(!CrmZohoCode.of(CrmZohoStatus.fromJson(mapOf("connected" to true))).usesSavedSecret)
    }
}
