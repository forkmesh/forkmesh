package com.forkmesh.nativeapp.phonesecurity

import java.net.URI
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.time.format.DateTimeParseException
import java.time.format.ResolverStyle
import java.time.temporal.ChronoUnit

enum class InjectedFamily(val label: String) { Frida("Frida"), Substrate("Substrate"), Xposed("Xposed") }

data class InjectedLibrary(val name: String, val family: InjectedFamily)

enum class RelayVerdict { Secure, LocalPlain, Plain, Unknown }

object PhoneSecurityParsers {
    val mountTargets: List<String> = listOf("/", "/system", "/vendor")

    private val ignoredRootTypes = setOf("rootfs", "tmpfs")

    fun writableSystemMounts(mounts: String): List<String> {
        val last = linkedMapOf<String, Pair<String, List<String>>>()
        for (line in mounts.lineSequence()) {
            val fields = line.trim().split(Regex("\\s+"))
            if (fields.size < 4) continue
            val point = fields[1].replace("\\040", " ")
            if (point !in mountTargets) continue
            last.remove(point)
            last[point] = fields[2] to fields[3].split(',')
        }
        return mountTargets.filter { point ->
            val entry = last[point] ?: return@filter false
            val (type, options) = entry
            if (point == "/" && type in ignoredRootTypes) return@filter false
            "rw" in options
        }
    }

    private val markers: List<Pair<String, InjectedFamily>> = listOf(
        "frida-agent" to InjectedFamily.Frida,
        "frida-gadget" to InjectedFamily.Frida,
        "libfrida" to InjectedFamily.Frida,
        "re.frida" to InjectedFamily.Frida,
        "libsubstrate" to InjectedFamily.Substrate,
        "xposedbridge" to InjectedFamily.Xposed,
        "liblspd" to InjectedFamily.Xposed,
        "libriru" to InjectedFamily.Xposed,
        "libedxp" to InjectedFamily.Xposed,
        "edxp" to InjectedFamily.Xposed,
    )

    private val segmentSeparators = Regex("[/: \\t\\[\\]]+")

    fun injectedLibraries(maps: String): List<InjectedLibrary> {
        val found = linkedMapOf<String, InjectedLibrary>()
        for (line in maps.lineSequence()) {
            val path = mapsPath(line) ?: continue
            for (segment in path.split(segmentSeparators)) {
                val lower = segment.lowercase()
                if (lower.isEmpty()) continue
                val family = markers.firstOrNull { lower.startsWith(it.first) }?.second ?: continue
                found.putIfAbsent(lower, InjectedLibrary(segment, family))
            }
        }
        return found.values.toList()
    }

    private fun mapsPath(line: String): String? {
        val fields = line.trim().split(Regex("\\s+"), limit = 6)
        if (fields.size < 6) return null
        return fields[5].trim().ifEmpty { null }
    }

    fun accessibilityPackages(enabled: String): List<String> =
        enabled.split(':')
            .map { it.trim().substringBefore('/').trim() }
            .filter { it.isNotEmpty() }
            .distinct()

    fun packageOfComponent(component: String): String = component.trim().substringBefore('/').trim()

    fun isEmulator(build: PhoneBuild): Boolean {
        val fingerprint = build.fingerprint.lowercase()
        val model = build.model.lowercase()
        val manufacturer = build.manufacturer.lowercase()
        val brand = build.brand.lowercase()
        val device = build.device.lowercase()
        val product = build.product.lowercase()
        val hardware = build.hardware.lowercase()
        return fingerprint.startsWith("generic") ||
            fingerprint.startsWith("unknown") ||
            fingerprint.contains("/sdk_gphone") ||
            fingerprint.contains("emulator") ||
            model.contains("google_sdk") ||
            model.contains("emulator") ||
            model.contains("android sdk built for") ||
            model.startsWith("sdk_gphone") ||
            manufacturer.contains("genymotion") ||
            (brand.startsWith("generic") && device.startsWith("generic")) ||
            product in setOf("sdk", "google_sdk", "sdk_x86", "sdk_x86_64", "vbox86p", "emulator", "simulator") ||
            product.startsWith("sdk_gphone") ||
            product.startsWith("sdk_google") ||
            hardware in setOf("goldfish", "ranchu", "vbox86", "nox", "ttvm_x86") ||
            hardware.startsWith("ranchu") ||
            hardware.startsWith("goldfish")
    }

    private val patchFormat: DateTimeFormatter = DateTimeFormatter.ofPattern("uuuu-MM-dd").withResolverStyle(ResolverStyle.STRICT)

    fun patchDate(text: String): LocalDate? =
        try {
            LocalDate.parse(text.trim(), patchFormat)
        } catch (_: DateTimeParseException) {
            null
        }

    fun patchAgeDays(text: String, today: LocalDate): Long? = patchDate(text)?.let { ChronoUnit.DAYS.between(it, today) }

    fun patchStatus(ageDays: Long): PhoneSecurityStatus = when {
        ageDays <= 90 -> PhoneSecurityStatus.Pass
        ageDays <= 180 -> PhoneSecurityStatus.Review
        ageDays <= 365 -> PhoneSecurityStatus.Warning
        else -> PhoneSecurityStatus.Critical
    }

    fun osStatus(sdkInt: Int): PhoneSecurityStatus = when {
        sdkInt >= 34 -> PhoneSecurityStatus.Pass
        sdkInt >= 31 -> PhoneSecurityStatus.Review
        else -> PhoneSecurityStatus.Warning
    }

    private val androidVersions = mapOf(
        21 to "5.0", 22 to "5.1", 23 to "6", 24 to "7.0", 25 to "7.1", 26 to "8.0", 27 to "8.1", 28 to "9", 29 to "10",
        30 to "11", 31 to "12", 32 to "12L", 33 to "13", 34 to "14", 35 to "15", 36 to "16", 37 to "17",
    )

    fun androidVersionName(sdkInt: Int): String =
        androidVersions[sdkInt]?.let { "Android $it (API $sdkInt)" } ?: "Android API $sdkInt"

    fun timeoutStatus(millis: Long): PhoneSecurityStatus = when {
        millis < 0 -> PhoneSecurityStatus.Warning
        millis <= 5 * 60_000L -> PhoneSecurityStatus.Pass
        millis <= 30 * 60_000L -> PhoneSecurityStatus.Review
        else -> PhoneSecurityStatus.Warning
    }

    fun durationText(millis: Long): String {
        if (millis < 0 || millis >= Int.MAX_VALUE / 2) return "never"
        val seconds = millis / 1000
        return when {
            seconds < 60 -> plural(seconds, "second")
            seconds < 3600 -> plural(seconds / 60, "minute")
            else -> plural(seconds / 3600, "hour")
        }
    }

    private fun plural(count: Long, unit: String) = if (count == 1L) "1 $unit" else "$count ${unit}s"

    fun installerName(installer: String): String? = PhoneSecurityPackages.trustedInstallers[installer.trim()]

    fun isTrustedInstaller(installer: String): Boolean = installerName(installer) != null

    fun isTrustedKeyboard(keyboardPackage: String): Boolean = keyboardPackage.trim() in PhoneSecurityPackages.trustedKeyboards

    fun keyboardName(keyboardPackage: String): String =
        PhoneSecurityPackages.trustedKeyboards[keyboardPackage.trim()] ?: keyboardPackage.trim()

    fun relayHost(url: String): String? {
        val uri = try {
            URI(url.trim())
        } catch (_: Exception) {
            return null
        }
        return uri.host?.removePrefix("[")?.removeSuffix("]")?.lowercase()?.ifEmpty { null }
    }

    fun relayScheme(url: String): String? =
        try {
            URI(url.trim()).scheme?.lowercase()
        } catch (_: Exception) {
            null
        }

    fun isLocalHost(host: String): Boolean {
        val value = host.trim().removePrefix("[").removeSuffix("]").lowercase().removeSuffix(".")
        if (value.isEmpty()) return false
        if (value == "localhost" || value.endsWith(".localhost")) return true
        if (value == "::1" || value == "0:0:0:0:0:0:0:1") return true
        if (value == "10.0.2.2") return true
        if (value.endsWith(".local")) return true
        val octets = value.split('.')
        return octets.size == 4 && octets[0] == "127" && octets.all { part -> part.toIntOrNull()?.let { it in 0..255 } == true }
    }

    fun relayVerdict(url: String): RelayVerdict {
        val scheme = relayScheme(url) ?: return RelayVerdict.Unknown
        val host = relayHost(url) ?: return RelayVerdict.Unknown
        return when (scheme) {
            "https", "wss" -> RelayVerdict.Secure
            "http", "ws" -> if (isLocalHost(host)) RelayVerdict.LocalPlain else RelayVerdict.Plain
            else -> RelayVerdict.Unknown
        }
    }

    fun commonName(distinguishedName: String): String {
        val parts = distinguishedName.split(Regex(",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)"))
        for (part in parts) {
            val key = part.substringBefore('=').trim()
            if (key.equals("CN", ignoreCase = true)) {
                return part.substringAfter('=').trim().removeSurrounding("\"").ifEmpty { distinguishedName.trim() }
            }
        }
        for (part in parts) {
            val key = part.substringBefore('=').trim()
            if (key.equals("O", ignoreCase = true)) return part.substringAfter('=').trim().removeSurrounding("\"")
        }
        return distinguishedName.trim()
    }

    fun encryptionStatus(status: Int): PhoneSecurityStatus? = when (status) {
        ENCRYPTION_ACTIVE, ENCRYPTION_ACTIVE_PER_USER -> PhoneSecurityStatus.Pass
        ENCRYPTION_ACTIVE_DEFAULT_KEY, ENCRYPTION_ACTIVATING -> PhoneSecurityStatus.Review
        ENCRYPTION_INACTIVE, ENCRYPTION_UNSUPPORTED -> PhoneSecurityStatus.Critical
        else -> null
    }

    const val ENCRYPTION_UNSUPPORTED = 0
    const val ENCRYPTION_INACTIVE = 1
    const val ENCRYPTION_ACTIVATING = 2
    const val ENCRYPTION_ACTIVE = 3
    const val ENCRYPTION_ACTIVE_DEFAULT_KEY = 4
    const val ENCRYPTION_ACTIVE_PER_USER = 5
}
