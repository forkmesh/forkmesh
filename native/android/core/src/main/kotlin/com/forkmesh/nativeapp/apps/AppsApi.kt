package com.forkmesh.nativeapp.apps

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.DartUri
import com.forkmesh.nativeapp.json.jsonText

open class AppsApi(private val api: ApiService) {
    open fun installed(): InstalledApps = InstalledApps.fromJson(api.accountApiCall("GET", "/api/apps/installed", null))

    open fun revoke(installId: String): String =
        jsonText(api.accountApiCall("DELETE", "/api/apps/installs/${DartUri.encodeComponent(installId)}", emptyMap())["revoked"])

    open fun decide(org: String, requestId: String, approve: Boolean): String = jsonText(
        api.accountApiCall(
            "POST",
            "/api/apps/orgs/${DartUri.encodeComponent(org.trim().lowercase())}/requests/${DartUri.encodeComponent(requestId)}",
            linkedMapOf("action" to if (approve) "approve" else "deny"),
        )["status"],
    )

    open fun commands(): AppCommands = AppCommands.fromJson(api.accountApiCall("GET", "/api/apps/commands", null))

    private fun reply(path: String, body: Map<String, Any?>): AppReply {
        val result = api.jsonExchange("POST", path, body = body)
        return AppReply.fromJson(result.statusCode, result.body)
    }

    open fun run(text: String, channelId: String): AppReply =
        reply("/api/apps/commands/run", linkedMapOf("text" to text, "channelId" to channelId))

    open fun interact(appMessageId: String, channelId: String, actionId: String, value: String): AppReply = reply(
        "/api/apps/interact",
        linkedMapOf("appMessageId" to appMessageId, "channelId" to channelId, "actionId" to actionId, "value" to value),
    )

    open fun forwardMentions(channelId: String, messageId: String, text: String): Int {
        val result = api.accountApiCall("POST", "/api/apps/mentions", linkedMapOf("channelId" to channelId, "messageId" to messageId, "text" to text))
        return (result["forwarded"] as? Number)?.toInt() ?: 0
    }
}
