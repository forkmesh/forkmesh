package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

enum class EventTicketStatus { OK, SIGNED_OUT, UNAVAILABLE }

data class EventTicket(val status: EventTicketStatus, val ticket: String = "")

data class ApiJsonResult(val statusCode: Int, val body: Map<String, Any?>) {
    val ok: Boolean get() = statusCode in 200..299
    val error: String get() = body.str("error")
}

data class PushRegistration(
    val ok: Boolean,
    val statusCode: Int = 0,
    val relayConfigured: Boolean? = null,
)

data class PushTestDelivery(
    val environment: String,
    val delivered: Boolean,
    val status: Int,
    val reason: String,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): PushTestDelivery {
            val status = json["status"]
            return PushTestDelivery(
                environment = json.str("environment"),
                delivered = json["delivered"] == true,
                status = if (status is Number) status.toDouble().toInt() else 0,
                reason = json.str("reason"),
            )
        }
    }
}

data class PushTestResult(
    val statusCode: Int,
    val ok: Boolean = false,
    val configured: Boolean? = null,
    val devices: Int = 0,
    val delivered: Int = 0,
    val results: List<PushTestDelivery> = emptyList(),
    val error: String = "",
    val configProblem: String = "",
) {
    val summary: String
        get() {
            if (statusCode == 0) {
                return "Could not reach the relay. Check the connection and try again."
            }
            if (statusCode == 401) return "The session has expired. Sign in again."
            if (statusCode == 404 || statusCode == 405) {
                return "This relay does not support push tests yet. " +
                    "Update the relay and try again."
            }
            if (statusCode == 429) return "Try again in a few seconds."
            if (error == "apns_provider_key") {
                return "The relay's APNs key is invalid. Ask the operator to check " +
                    "APNS_AUTH_KEY and APNS_KEY_ID."
            }
            if (error == "device_lookup_failed") {
                return "The relay could not read this account's devices. Try again."
            }
            if (statusCode < 200 || statusCode >= 300) {
                return "The relay answered HTTP $statusCode. Try again later."
            }
            if (configProblem == "key_id_malformed" || configProblem == "team_id_malformed") {
                val name = if (configProblem == "key_id_malformed") "APNS_KEY_ID" else "APNS_TEAM_ID"
                return "The relay's $name is not a valid Apple identifier (ten " +
                    "characters, A-Z and 0-9), so Apple refuses every push. Ask the " +
                    "operator to correct it."
            }
            if (configured == false) {
                return "This relay has no APNs credentials, so it cannot send pushes. " +
                    "Ask the operator to set APNS_AUTH_KEY, APNS_KEY_ID and " +
                    "APNS_TEAM_ID."
            }
            if (devices == 0) {
                return "This phone is not registered for pushes yet. " +
                    "Allow notifications and try again."
            }
            if (delivered > 0) {
                val sandbox = results.filter { it.delivered }.all { it.environment == "development" }
                val where = if (sandbox) " through the APNs sandbox" else ""
                return "Sent to $delivered of $devices " +
                    "device${if (devices == 1) "" else "s"}$where. " +
                    "A ForkMesh banner should arrive within a few seconds."
            }
            val failure = results.firstOrNull { !it.delivered }
                ?: PushTestDelivery(environment = "", delivered = false, status = 0, reason = "")
            if (failure.status == 0) {
                return "The relay could not reach Apple's gateway. " +
                    "Try again in a moment."
            }
            val reason = if (failure.reason.isEmpty()) "HTTP ${failure.status}" else failure.reason
            return "Apple rejected the push ($reason).${hint(failure.reason)}"
        }

    companion object {
        fun unreachable(): PushTestResult = PushTestResult(statusCode = 0)

        fun fromJson(statusCode: Int, body: Map<String, Any?>): PushTestResult {
            val configured = body["configured"]
            val devices = body["devices"]
            val delivered = body["delivered"]
            val raw = body["results"]
            return PushTestResult(
                statusCode = statusCode,
                ok = body["ok"] == true,
                configured = configured as? Boolean,
                devices = if (devices is Number) devices.toDouble().toInt() else 0,
                delivered = if (delivered is Number) delivered.toDouble().toInt() else 0,
                results = if (raw is List<*>) jsonMaps(raw).map(PushTestDelivery::fromJson) else emptyList(),
                error = body.str("error"),
                configProblem = body.str("configProblem"),
            )
        }

        private fun hint(reason: String): String = when (reason) {
            "BadDeviceToken" ->
                " The token was minted for the other APNs environment (sandbox versus " +
                    "production), so the relay pruned it; reopen the app to register " +
                    "again."
            "DeviceTokenNotForTopic", "TopicDisallowed" ->
                " The relay's APNs key or topic does not match this app's bundle id."
            "InvalidProviderToken", "MissingProviderToken", "ExpiredProviderToken" ->
                " The relay's APNS_KEY_ID or APNS_TEAM_ID does not match its key."
            "Unregistered" ->
                " Apple no longer knows this token; reopen the app to register again."
            else -> ""
        }
    }
}

data class BugReportResult(val ok: Boolean, val id: String, val message: String) {
    companion object {
        fun success(id: String) = BugReportResult(ok = true, id = id, message = "")

        fun failure(message: String) = BugReportResult(ok = false, id = "", message = message)
    }
}

data class BugReportAttachment(val name: String, val contentType: String, val data: ByteArray)

data class BugReportInboxAttachment(val id: String, val name: String, val contentType: String, val size: Long) {
    companion object {
        fun from(json: Map<String, Any?>) = BugReportInboxAttachment(
            json.str("id"), json.str("name", "attachment"), json.str("contentType"), json.long("size"),
        )
    }
}

data class BugReportInboxItem(
    val id: String,
    val status: String,
    val severity: String,
    val source: String,
    val platform: String,
    val appVersion: String,
    val osVersion: String,
    val device: String,
    val createdAt: Long,
    val account: String,
    val message: String,
    val contact: String,
    val screen: String,
    val packageId: String,
    val attachments: List<BugReportInboxAttachment>,
) {
    companion object {
        fun from(json: Map<String, Any?>) = BugReportInboxItem(
            id = json.str("id"),
            status = json.str("status", "new"),
            severity = json.str("severity", "normal"),
            source = json.str("source"),
            platform = json.str("platform"),
            appVersion = json.str("appVersion"),
            osVersion = json.str("osVersion"),
            device = json.str("device"),
            createdAt = json.long("createdAt"),
            account = json.str("account"),
            message = json.str("message"),
            contact = json.str("contact"),
            screen = json.str("screen"),
            packageId = json.str("package"),
            attachments = json.maps("attachments").map(BugReportInboxAttachment::from),
        )
    }
}

data class BugReportInbox(
    val ok: Boolean,
    val reports: List<BugReportInboxItem> = emptyList(),
    val counts: Map<String, Int> = emptyMap(),
    val nextOffset: Int? = null,
    val message: String = "",
) {
    companion object {
        fun from(json: Map<String, Any?>): BugReportInbox {
            val counts = json.obj("counts")
            return BugReportInbox(
                ok = true,
                reports = json.maps("reports").map(BugReportInboxItem::from),
                counts = counts.keys.associateWith { counts.int(it) },
                nextOffset = if (json["nextOffset"] == null) null else json.int("nextOffset"),
            )
        }

        fun failure(message: String) = BugReportInbox(ok = false, message = message)
    }
}

data class BugReportStatusChange(
    val ok: Boolean,
    val status: String = "",
    val counts: Map<String, Int> = emptyMap(),
    val message: String = "",
) {
    companion object {
        fun failure(message: String) = BugReportStatusChange(ok = false, message = message)
    }
}

class BugReportFile(val ok: Boolean, val data: ByteArray = ByteArray(0), val contentType: String = "", val message: String = "") {
    companion object {
        fun failure(message: String) = BugReportFile(ok = false, message = message)
    }
}
