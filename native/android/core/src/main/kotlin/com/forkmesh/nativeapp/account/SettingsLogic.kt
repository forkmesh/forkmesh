package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.auth.AccountDevice
import com.forkmesh.nativeapp.model.auth.AuthException
import com.forkmesh.nativeapp.model.auth.AuthSession
import java.net.URI

fun sessionAccountName(session: AuthSession?): String {
    val nodeName = session?.nodeName.orEmpty()
    return if (nodeName.isNotEmpty()) nodeName else session?.email.orEmpty()
}

fun hasRealSession(session: AuthSession?): Boolean = session != null && session.sessionKind != "preview"

fun showsUserStatus(session: AuthSession?, guest: Boolean): Boolean = !guest && hasRealSession(session)

fun adoptedDisplayName(fieldText: String, session: AuthSession?): String? {
    val accountName = sessionAccountName(session)
    if (accountName.isEmpty()) return null
    return if (fieldText.isEmpty() || fieldText == "preview-node") accountName else null
}

data class ProfileCardText(val title: String, val subtitle: String)

fun profileCardText(session: AuthSession?, guest: Boolean): ProfileCardText {
    val preview = session?.sessionKind == "preview"
    val nodeName = session?.nodeName.orEmpty()
    val email = session?.email.orEmpty()
    val title = when {
        guest -> "Guest"
        preview -> "Preview mode"
        nodeName.isNotEmpty() -> nodeName
        email.isNotEmpty() -> email
        else -> "Not signed in"
    }
    val subtitle = when {
        guest -> "Exploring read-only without an account"
        preview -> "This is local preview data. Sign out and log in to show your Worker account."
        email.isNotEmpty() -> email
        else -> "No Worker account session found on this device"
    }
    return ProfileCardText(title, subtitle)
}

data class FundingStatus(val connected: Boolean, val title: String, val detail: String)

fun fundingStatus(session: AuthSession?): FundingStatus {
    val connected = session?.hasPayoutAddress == true
    val address = session?.solana?.trim().orEmpty()
    return FundingStatus(
        connected = connected,
        title = if (connected) "Payout address connected" else "No payout address connected",
        detail = address.ifEmpty {
            "Add a public Solana address only if you want optional mirror-reward or direct-donation metadata. " +
                "Legacy bounty funding is frozen."
        },
    )
}

sealed class ThisDeviceCard {
    data object SignInToRegister : ThisDeviceCard()

    data object NotRegistered : ThisDeviceCard()

    data class MobileNode(
        val label: String,
        val deviceId: String,
        val capabilities: String,
        val lastSeen: String,
        val canServeAsNode: Boolean,
    ) : ThisDeviceCard()
}

fun thisDeviceCard(session: AuthSession?, nowMillis: Long = System.currentTimeMillis()): ThisDeviceCard {
    if (session == null || session.sessionKind == "preview") return ThisDeviceCard.SignInToRegister
    if (session.deviceId.isEmpty() && !session.isMobileNode) return ThisDeviceCard.NotRegistered
    val device = session.thisDevice
    val label = device?.label?.trim()?.takeIf { it.isNotEmpty() } ?: "This phone"
    return ThisDeviceCard.MobileNode(
        label = label,
        deviceId = shortDeviceId(session.deviceId),
        capabilities = device?.capabilitySummary ?: capabilitySummary(session.capabilities),
        lastSeen = if (device != null && device.lastSeen > 0) relativeTime(device.lastSeen, nowMillis) else "now",
        canServeAsNode = session.isMobileNode,
    )
}

fun capabilitySummary(capabilities: List<String>): String {
    val parts = ArrayList<String>()
    if ("publish_repo" in capabilities) parts.add("Publishes")
    if ("owner_sign" in capabilities) parts.add("signs as owner")
    if (parts.isEmpty()) return "Browse only"
    val text = parts.joinToString(" · ")
    return text.substring(0, 1).uppercase() + text.substring(1)
}

fun shortDeviceId(id: String): String = when {
    id.isEmpty() -> "—"
    id.length <= 10 -> id
    else -> id.substring(0, 6) + "…" + id.substring(id.length - 4)
}

fun shortDeviceKey(key: String): String =
    if (key.length <= 18) key else key.substring(0, 9) + "..." + key.substring(key.length - 6)

fun countLabel(n: Int, noun: String): String = "$n $noun${if (n == 1) "" else "s"}"

data class NodesSummary(val devices: List<AccountDevice>, val desktopCount: Int, val mobileCount: Int) {
    val headline: String
        get() = if (devices.isEmpty()) {
            "No nodes registered yet"
        } else {
            "${countLabel(desktopCount, "desktop node")} · ${countLabel(mobileCount, "mobile node")}"
        }

    val showsDesktopNote: Boolean get() = desktopCount > 0
}

fun nodesSummary(refreshed: List<AccountDevice>?, session: AuthSession): NodesSummary {
    val devices = refreshed ?: session.devices
    return NodesSummary(
        devices = devices,
        desktopCount = devices.count { it.isDesktopNode && !it.isRevoked },
        mobileCount = devices.count { it.isMobileNode && !it.isRevoked },
    )
}

fun nodesEmptyText(loading: Boolean): String = if (loading) {
    "Loading nodes…"
} else {
    "Log in from the ForkMesh desktop app or another phone with this account to register more nodes."
}

fun isCurrentDevice(device: AccountDevice, session: AuthSession): Boolean =
    device.current || (session.deviceId.isNotEmpty() && device.id == session.deviceId)

enum class DeviceTone { Offline, Success, Warning }

fun deviceTone(device: AccountDevice): DeviceTone = when {
    device.isRevoked -> DeviceTone.Offline
    device.canOwnerSign -> DeviceTone.Success
    else -> DeviceTone.Warning
}

fun deviceRowTitle(device: AccountDevice, isCurrent: Boolean): String =
    if (isCurrent) "${device.displayName} (this device)" else device.displayName

fun deviceRowDetail(device: AccountDevice): String = "${device.kindLabel} · ${device.capabilityLabel}"

fun deviceRowTimeline(device: AccountDevice, nowMillis: Long = System.currentTimeMillis()): String? = when {
    device.revokedAt > 0 -> "Revoked ${relativeTime(device.revokedAt, nowMillis)}"
    device.lastSeen > 0 -> "Last seen ${relativeTime(device.lastSeen, nowMillis)}"
    else -> null
}

fun webSettingsUrl(serverUrl: String): String {
    val uri = try {
        URI(serverUrl.trim())
    } catch (_: Exception) {
        null
    }
    val host = uri?.host
    if (uri == null || host.isNullOrEmpty()) return "https://app.forkmesh.com/dashboard/settings"
    val scheme = if (uri.scheme?.lowercase() == "ws") "http" else "https"
    val port = uri.port
    val showPort = port >= 0 && !((scheme == "http" && port == 80) || (scheme == "https" && port == 443))
    return "$scheme://${host.lowercase()}${if (showPort) ":$port" else ""}/dashboard/settings"
}

fun showsPushCard(supported: Boolean, guest: Boolean): Boolean = supported && !guest

fun serveAsNodeNote(supported: Boolean, relayLabel: String): String = if (supported) {
    "Read this account's inbox and apply submissions while the app is open. Nothing runs in the " +
        "background, and a desktop node always takes priority."
} else {
    "${relayLabel.ifEmpty { "This relay" }} does not serve the node drain, so this phone cannot act as a node there. " +
        "Switch to a relay that does."
}

fun pushPermissionLabel(permission: String): String = when (permission) {
    "authorized" -> "Allowed"
    "provisional" -> "Allowed quietly (delivered to Notification Center)"
    "ephemeral" -> "Allowed for this App Clip session"
    "denied" -> "Turned off in iOS Settings"
    "notDetermined" -> "Not asked yet"
    else -> "Not checked yet"
}

fun pushTokenLabel(token: String, permission: String, environment: String, registered: Boolean): String {
    if (token.isEmpty()) return if (permission == "denied") "Not requested" else "Waiting for Apple to issue one"
    val gateway = if (environment == "development") "APNs sandbox" else "APNs production"
    return if (registered) "Registered with the relay ($gateway)" else "Issued, not yet registered ($gateway)"
}

fun pushRelayLabel(relayConfigured: Boolean?): String = when (relayConfigured) {
    true -> "Can send pushes"
    false -> "No APNs credentials configured"
    null -> "Unknown until the next registration"
}

fun pushCanEnable(token: String, permission: String): Boolean = token.isEmpty() && permission != "denied"

fun pushSendLabel(sending: Boolean): String = if (sending) "Sending…" else "Send test notification"

fun signedInAsName(session: AuthSession?): String {
    val nodeName = session?.nodeName.orEmpty()
    return if (nodeName.isNotEmpty()) nodeName else session?.email ?: "Current account"
}

enum class AccountCard { Guest, SignOut }

fun accountCard(guest: Boolean): AccountCard = if (guest) AccountCard.Guest else AccountCard.SignOut

fun showsDeleteAccount(session: AuthSession?): Boolean = hasRealSession(session)

fun deleteAccountConfirmError(confirmText: String): String? =
    if (confirmText.trim() != "DELETE") "Type DELETE to confirm account deletion." else null

fun deleteAccountFailure(error: Throwable): String =
    if (error is AuthException) error.message else "Could not delete account. Check your password and try again."
