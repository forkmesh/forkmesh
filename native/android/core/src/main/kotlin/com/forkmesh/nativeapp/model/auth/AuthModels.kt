package com.forkmesh.nativeapp.model.auth

import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.model.modelInt

private fun dartText(value: Any?): String = if (value == null) "null" else jsonText(value)

private fun capabilityList(value: Any?): List<String> = when (value) {
    is List<*> -> value.map(::dartText)
    is String -> value.split(',').filter { it.isNotEmpty() }
    else -> emptyList()
}

data class AuthSession(
    val nodeName: String,
    val email: String,
    val status: String,
    val pubkey: String,
    val emailVerified: Boolean,
    val isAdmin: Boolean,
    val solana: String,
    val hasPayoutAddress: Boolean,
    val avatarPng: String,
    val avatarUpdatedAt: Long,
    val createdAt: Long,
    val savedAt: Long,
    val sessionKind: String = "account",
    val deviceKind: String = "",
    val deviceId: String = "",
    val desktopCapable: Boolean = false,
    val capabilities: List<String> = emptyList(),
    val devices: List<AccountDevice> = emptyList(),
    val sessionToken: String = "",
    val userStatus: UserStatus = UserStatus(),
    val chatRetention: ChatRetention? = null,
) {
    val canSubmitIssue: Boolean get() = "submit_issue" in capabilities
    val canSubmitPr: Boolean get() = "submit_pr" in capabilities
    val canHost: Boolean get() = "host_repo" in capabilities
    val canPublish: Boolean get() = "publish_repo" in capabilities
    val canOwnerSign: Boolean get() = "owner_sign" in capabilities

    val isMobileNode: Boolean get() = sessionKind == "mobile_node" && canOwnerSign

    val desktopDevices: List<AccountDevice> get() = devices.filter { it.kind == "desktop_node" && it.enabled }
    val mobileDevices: List<AccountDevice> get() = devices.filter { it.kind == "mobile_node" && it.enabled }

    val thisDevice: AccountDevice?
        get() {
            if (deviceId.isEmpty()) return null
            return devices.firstOrNull { it.current || it.id == deviceId }
        }

    fun toJson(): LinkedHashMap<String, Any?> = linkedMapOf<String, Any?>(
        "nodeName" to nodeName,
        "email" to email,
        "status" to status,
        "pubkey" to pubkey,
        "emailVerified" to emailVerified,
        "isAdmin" to isAdmin,
        "solana" to solana,
        "hasPayoutAddress" to hasPayoutAddress,
        "avatarPng" to avatarPng,
        "avatarUpdatedAt" to avatarUpdatedAt,
        "createdAt" to createdAt,
        "at" to savedAt,
        "sessionKind" to sessionKind,
        "deviceKind" to deviceKind,
        "deviceId" to deviceId,
        "desktopCapable" to desktopCapable,
        "capabilities" to capabilities,
        "devices" to devices.map { it.toJson() },
        "sessionToken" to sessionToken,
        "userStatus" to userStatus.toJson(),
    ).also { json -> chatRetention?.let { json["chatRetention"] = it.toJson() } }

    override fun toString(): String =
        "AuthSession(nodeName=$nodeName, email=$email, sessionKind=$sessionKind, deviceId=$deviceId, " +
            "capabilities=$capabilities, devices=${devices.size})"

    companion object {
        fun fromJson(json: Map<String, Any?>): AuthSession {
            val nodeName = json["nodeName"] ?: json["name"]
            return AuthSession(
                nodeName = if (nodeName == null) "" else jsonText(nodeName),
                email = json.str("email"),
                status = json.str("status"),
                pubkey = json.str("pubkey"),
                emailVerified = json["emailVerified"] == true,
                isAdmin = json["isAdmin"] == true,
                solana = json.str("solana"),
                hasPayoutAddress = json["hasPayoutAddress"] == true,
                avatarPng = json.str("avatarPng"),
                avatarUpdatedAt = modelInt(json["avatarUpdatedAt"]),
                createdAt = modelInt(json["createdAt"]),
                savedAt = json["at"]?.let(::modelInt) ?: System.currentTimeMillis(),
                sessionKind = json.str("sessionKind", "account"),
                deviceKind = json.str("deviceKind"),
                deviceId = json.str("deviceId"),
                desktopCapable = json["desktopCapable"] == true,
                capabilities = (json["capabilities"] as? List<*>)?.map(::dartText) ?: emptyList(),
                devices = if (json["devices"] is List<*>) jsonMaps(json["devices"]).map(AccountDevice::fromJson) else emptyList(),
                sessionToken = json.str("sessionToken"),
                userStatus = UserStatus.fromJson(json["userStatus"]),
                chatRetention = ChatRetention.fromJsonOrNull(json["chatRetention"]),
            )
        }
    }
}

data class AccountDevice(
    val id: String,
    val pubkey: String,
    val kind: String,
    val label: String,
    val capabilities: List<String>,
    val enabled: Boolean,
    val lastSeen: Long,
    val createdAt: Long = 0,
    val revokedAt: Long = 0,
    val current: Boolean = false,
) {
    val canOwnerSign: Boolean get() = "owner_sign" in capabilities
    val canHost: Boolean get() = "host_repo" in capabilities
    val canPublish: Boolean get() = "publish_repo" in capabilities
    val isRevoked: Boolean get() = revokedAt > 0 || !enabled
    val isMobileNode: Boolean get() = kind == "mobile_node"
    val isDesktopNode: Boolean get() = kind == "desktop_node"

    val displayName: String get() = if (label.trim().isNotEmpty()) label.trim() else kindLabel

    val kindLabel: String
        get() = when (kind) {
            "desktop_node" -> "Desktop node"
            "mobile_node" -> "Mobile node"
            "mobile_client" -> "Mobile client"
            else -> "Device"
        }

    val statusLabel: String
        get() = when {
            revokedAt > 0 -> "Revoked"
            !enabled -> "Disabled"
            canOwnerSign -> "Ready for signed controls"
            else -> "Browse only"
        }

    val capabilitySummary: String
        get() {
            val parts = ArrayList<String>()
            if (canHost) parts.add("Hosts")
            if (canPublish) parts.add("Publishes")
            if (canOwnerSign) parts.add("signs as owner")
            if (parts.isEmpty()) return "Browse only"
            val text = parts.joinToString(" · ")
            return text.substring(0, 1).uppercase() + text.substring(1)
        }

    val shortId: String
        get() {
            val clean = id.trim()
            if (clean.length <= 10) return clean
            return clean.substring(0, 6) + "…" + clean.substring(clean.length - 4)
        }

    val capabilityLabel: String
        get() {
            val labels = ArrayList<String>()
            if (canHost) labels.add("Host repo")
            if (canPublish) labels.add("Publish repo")
            if (canOwnerSign) labels.add("Owner signing")
            if (labels.isEmpty()) labels.addAll(capabilities.map(::capabilityTitle))
            return if (labels.isEmpty()) "No capabilities" else labels.joinToString(" · ")
        }

    fun toJson(): LinkedHashMap<String, Any?> = linkedMapOf(
        "id" to id,
        "pubkey" to pubkey,
        "kind" to kind,
        "label" to label,
        "capabilities" to capabilities,
        "enabled" to enabled,
        "lastSeen" to lastSeen,
        "createdAt" to createdAt,
        "revokedAt" to revokedAt,
        "current" to current,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): AccountDevice {
            val enabled = json["enabled"]
            val disabled = enabled == false || (enabled is Number && enabled.toDouble() == 0.0)
            return AccountDevice(
                id = json.str("id"),
                pubkey = json.str("pubkey"),
                kind = json.str("kind"),
                label = json.str("label"),
                capabilities = capabilityList(json["capabilities"]),
                enabled = !disabled,
                lastSeen = modelInt(json["lastSeen"] ?: json["last_seen"]),
                createdAt = modelInt(json["createdAt"] ?: json["created_at"]),
                revokedAt = modelInt(json["revokedAt"] ?: json["revoked_at"]),
                current = json["current"] == true,
            )
        }

        private fun capabilityTitle(raw: String): String {
            val words = raw.split('_').filter { it.isNotEmpty() }
            if (words.isEmpty()) return raw
            return words.joinToString(" ") { it.substring(0, 1).uppercase() + it.substring(1) }
        }
    }
}

data class AccountAvailability(
    val ok: Boolean,
    val exists: Boolean,
    val available: Boolean,
    val name: String,
    val message: String = "",
)

class AuthException(val code: String, override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

fun authErrorMessage(code: String): String = when (code) {
    "bad_totp" -> "Enter your authenticator code."
    "too_many_attempts" -> "Too many failed attempts. Wait a few minutes and try again."
    "account_disabled" -> "This account has been disabled."
    "invalid_credentials" -> "Incorrect email, username, or password."
    "pubkey_mismatch" -> "This account is linked to a different device key."
    "device_proof_required" ->
        "Could not prove this phone's key to the server. Check the device clock and try again."
    "device_key_conflict" ->
        "This phone's key is already registered to another account. Try again to sign in with a fresh key."
    "invalid_device_kind" ->
        "The server does not accept this app as a mobile node yet. Update the app or the server."
    "device_revoked" -> "This device was signed out from the web."
    "account_pending_verification" -> "This account is pending verification. Check your email and try again."
    "invalid_node_name" -> "Choose a valid username."
    "valid_email_required" -> "Enter a valid email address."
    "password_too_short" -> "Password must be at least 8 characters."
    "node_name_taken" -> "That username is already taken."
    "email_taken" -> "That email is already registered."
    "invalid_json" -> "Could not send the request. Please try again."
    "password_required" -> "Enter your current password to delete this account."
    "not_authenticated" -> "Sign in with a real account before deleting it."
    "bad_password" -> "Incorrect password. Check it and try again."
    "invalid_password" -> "Incorrect password. Check it and try again."
    else -> "Could not complete the request. Please try again."
}

fun deviceBindCanonical(account: String, pubkey: String, ts: String): String =
    "forkmesh-device-bind-v1\n$account\n$pubkey\n$ts"

fun joinDeviceLabel(device: String, os: String, fallback: String): String {
    val d = device.trim()
    val o = os.trim()
    if (d.isEmpty() && o.isEmpty()) return fallback
    if (d.isEmpty()) return o
    if (o.isEmpty()) return d
    val label = "$d · $o"
    return if (label.length > 120) label.substring(0, 120) else label
}

fun androidDeviceLabel(manufacturer: String, model: String, release: String): String {
    val brand = manufacturer.trim()
    val cleanModel = model.trim()
    val device = if (cleanModel.lowercase().startsWith(brand.lowercase())) cleanModel else "$brand $cleanModel".trim()
    return joinDeviceLabel(device, "Android $release", "Mobile device")
}
