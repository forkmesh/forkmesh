package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.coves.CoveJson
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

data class AccountExportFile(val fileName: String, val json: String, val devices: Int, val sshKeys: Int)

object AccountExport {
    const val PATH = "/api/accounts/export"
    const val SCHEMA = "forkmesh-account-export-v1"
    val secretFields = setOf("pass_hash", "pass_salt", "totp_secret", "avatar_png", "sessionToken", "seed")
    private val DAY = DateTimeFormatter.ofPattern("yyyy-MM-dd").withZone(ZoneOffset.UTC)

    fun file(map: Map<String, Any?>, account: String, nowMillis: Long): AccountExportFile {
        if (map.text("schema") != SCHEMA) throw AccountEndpointException("unsupported_export")
        val cleaned = LinkedHashMap(map)
        cleaned.remove("ok")
        cleaned["account"] = map.obj("account").filterKeys { it !in secretFields }
        val stem = account.lowercase().filter { it.isLetterOrDigit() || it == '-' }
        return AccountExportFile(
            fileName = "forkmesh-${stem.ifEmpty { "account" }}-export-${DAY.format(Instant.ofEpochMilli(nowMillis))}.json",
            json = CoveJson.pretty(cleaned, indent = "  "),
            devices = map.list("devices").size,
            sshKeys = map.list("sshKeys").size,
        )
    }
}

data class MembershipStatus(val configured: Boolean, val connected: Boolean, val tier: String, val patronStatus: String, val linkedAt: Long) {
    val title: String
        get() {
            if (!connected) return "Core (free)"
            val tierName = if (tier.isEmpty()) "Member" else tier.take(1).uppercase() + tier.drop(1)
            return if (tier == "core") "Patreon connected" else "$tierName member"
        }

    val detail: String
        get() {
            if (!connected) {
                return if (configured) "Membership is managed on forkmesh.com with Patreon." else "Membership is not offered on this server."
            }
            return when (patronStatus) {
                "active_patron" -> "Active pledge on Patreon."
                "declined_patron" -> "Patreon declined the last payment."
                "former_patron" -> "Former patron."
                else -> if (patronStatus.isEmpty()) "Linked to Patreon." else patronStatus.replace('_', ' ')
            }
        }

    companion object {
        val unknown = MembershipStatus(false, false, "core", "", 0)

        fun parse(map: Map<String, Any?>): MembershipStatus = MembershipStatus(
            configured = map.flag("configured"),
            connected = map.flag("connected"),
            tier = map.text("tier", "core").take(16),
            patronStatus = map.text("patronStatus").take(64),
            linkedAt = map.long("linkedAt"),
        )
    }
}

data class SecurityReportDraft(val title: String = "", val body: String = "", val component: String = "client", val contact: String = "") {
    val problem: String?
        get() = when {
            title.isBlank() -> "Add a short title."
            body.isBlank() -> "Describe the vulnerability and how to reproduce it."
            components.none { it.first == component } -> "Pick a component."
            body.length > MAX_BODY -> "The description is too long."
            else -> null
        }

    val payload: Map<String, Any?>
        get() = linkedMapOf(
            "title" to title.trim().take(MAX_TITLE),
            "body" to body.trim().take(MAX_BODY),
            "component" to component,
            "contact" to contact.trim().take(MAX_CONTACT),
        )

    companion object {
        val components = listOf(
            "identity" to "Identity keys and signing",
            "relay" to "Relay",
            "mirroring" to "Mirroring",
            "chat" to "Encrypted chat",
            "donations" to "Donations",
            "website" to "Website and dashboard",
            "client" to "Apps (desktop, phone)",
            "other" to "Something else",
        )
        const val PATH = "/api/security/report"
        const val MAX_TITLE = 200
        const val MAX_BODY = 16_384
        const val MAX_CONTACT = 254
    }
}

class AccountDataModel(private val client: AccountEndpointCalling, private val clock: () -> Long = System::currentTimeMillis) {
    @Volatile var exportFile: AccountExportFile? = null
        private set
    @Volatile var membership: MembershipStatus? = null
        private set
    @Volatile var busy = false
        private set
    @Volatile var error = ""
        private set
    @Volatile var reportSent = false
        private set
    @Volatile var report = SecurityReportDraft()

    fun prepareExport(account: String): AccountExportFile? {
        busy = true
        return try {
            val file = AccountExport.file(client.accountCall("GET", AccountExport.PATH, null), account, clock())
            exportFile = file
            error = ""
            file
        } catch (failure: AccountEndpointException) {
            error = if (failure.status == 404 || failure.status == 405) {
                "This server cannot export account data yet."
            } else {
                AccountErrorText.text(failure, "Could not export your account data.")
            }
            null
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not export your account data.")
            null
        } finally {
            busy = false
        }
    }

    fun clearExport() {
        exportFile = null
    }

    fun loadMembership() {
        membership = try {
            MembershipStatus.parse(client.accountCall("GET", "/api/accounts/patreon", null))
        } catch (_: Exception) {
            MembershipStatus.unknown
        }
    }

    fun sendReport(): Boolean {
        report.problem?.let {
            error = it
            return false
        }
        busy = true
        return try {
            client.accountCall("POST", SecurityReportDraft.PATH, report.payload)
            report = SecurityReportDraft()
            reportSent = true
            error = ""
            true
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not send the report. Email security@forkmesh.com instead.")
            false
        } finally {
            busy = false
        }
    }
}
