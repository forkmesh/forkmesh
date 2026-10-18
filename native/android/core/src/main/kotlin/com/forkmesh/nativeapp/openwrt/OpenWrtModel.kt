package com.forkmesh.nativeapp.openwrt

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.net.URI
import java.security.MessageDigest

object OpenWrt {
    const val PACKAGE_ID = "openwrt"
    val ANONYMOUS_SESSION = "0".repeat(32)
    const val SESSION_TIMEOUT = 3600
    const val ROLLBACK_SECONDS = 90
    const val STATUS_OK = 0
    const val STATUS_NOT_FOUND = 4
    const val STATUS_NO_DATA = 5
    const val STATUS_PERMISSION_DENIED = 6
    const val STATUS_TIMEOUT = 7
}

data class OpenWrtResult(
    val ok: Boolean = false,
    val status: Int = -1,
    val data: JsonMap = emptyMap(),
    val error: String = "",
    val accessDenied: Boolean = false,
    val unreachable: Boolean = false,
    val timedOut: Boolean = false,
    val certificate: String = "",
) {
    companion object {
        fun failure(message: String) = OpenWrtResult(error = message)
        val OK = OpenWrtResult(ok = true, status = 0)
    }
}

class OpenWrtProblem(message: String) : Exception(message)

data class OpenWrtAddress(val base: String = "", val host: String = "", val port: Int = -1, val error: String = "", val encrypted: Boolean = false) {
    val valid: Boolean get() = error.isEmpty() && base.isNotEmpty()
}

data class UciSection(val name: String, val type: String, val anonymous: Boolean = false, val index: Int = 0, val options: JsonMap = emptyMap()) {
    fun option(key: String, fallback: String = ""): String = if (options.containsKey(key)) OpenWrtModel.optionText(options[key]) else fallback

    fun flag(key: String, fallback: Boolean = false): Boolean =
        if (!options.containsKey(key)) fallback else option(key).lowercase() in setOf("1", "on", "true", "yes", "enabled")

    fun isList(key: String): Boolean = options[key] is List<*>
}

data class UciEdit(val values: Map<String, Any?> = emptyMap(), val removed: List<String> = emptyList())

data class OpenWrtLease(val expires: Long, val mac: String, val ip: String, val hostname: String)

data class OpenWrtPackage(val name: String, val version: String)

data class OpenWrtPackageTool(val program: String, val wrapper: Boolean)

data class OpenWrtService(val name: String, val enabled: Boolean, val running: Boolean)

data class OpenWrtInterfaceStatus(
    val name: String,
    val proto: String,
    val device: String,
    val up: Boolean,
    val uptime: Long,
    val ipv4: List<String>,
    val ipv6: List<String>,
    val dns: List<String>,
    val gateway: String,
)

data class OpenWrtChoice(val value: String, val label: String)

data class OpenWrtBlockDevice(
    val name: String,
    val device: String,
    val uuid: String = "",
    val label: String = "",
    val type: String = "",
    val mount: String = "",
    val size: Long = 0,
)

data class OpenWrtMountPoint(val device: String, val mount: String, val type: String, val size: Long, val avail: Long)

data class OpenWrtDisk(
    val block: OpenWrtBlockDevice,
    val mount: String = "",
    val size: Long = 0,
    val avail: Long = 0,
    val usageKnown: Boolean = false,
    val fstabSection: String = "",
    val bootTarget: String = "",
    val bootEnabled: Boolean = false,
) {
    val mounted: Boolean get() = mount.isNotEmpty()
    val title: String get() = if (block.label.isEmpty()) block.name else "${block.label} (${block.name})"
}

data class OpenWrtStorageCheck(
    val packagesKnown: Boolean = false,
    val installed: List<String> = emptyList(),
    val blockTool: Boolean = false,
    val fstabExists: Boolean = false,
    val autoMount: Boolean = false,
    val autoMountPending: Boolean = false,
    val usbKnown: Boolean = false,
    val usbControllers: Int = 0,
    val usbDevices: List<String> = emptyList(),
    val partitionsKnown: Boolean = false,
    val partitions: List<String> = emptyList(),
    val disks: Int = 0,
    val mounted: Int = 0,
    val listProblem: String = "",
    val kernelLines: List<String> = emptyList(),
)

data class OpenWrtShare(val section: String, val name: String, val path: String, val readOnly: Boolean, val guests: Boolean)

object OpenWrtModel {
    fun callBody(id: Int, session: String, objectName: String, method: String, args: Map<String, Any?> = emptyMap()): ByteArray =
        Json.stringify(
            linkedMapOf(
                "jsonrpc" to "2.0",
                "id" to id.toLong(),
                "method" to "call",
                "params" to listOf(session.ifEmpty { OpenWrt.ANONYMOUS_SESSION }, objectName, method, args),
            ),
        ).toByteArray(Charsets.UTF_8)

    fun statusMessage(status: Int): String = when (status) {
        0 -> "Done."
        1 -> "The router did not recognise the command."
        2 -> "The router rejected a value as invalid."
        3 -> "The router does not offer this function. It may need an extra package (LuCI provides most of them)."
        4 -> "The router could not find what was asked for."
        5 -> "The router had nothing to return."
        6 -> "The router refused: the session expired or this account is not allowed to do that."
        7 -> "The router took too long to answer."
        8 -> "The router does not support this."
        9 -> "The router reported an unknown error."
        10 -> "A service on the router is not running."
        else -> "The router answered with error $status."
    }

    @Suppress("UNCHECKED_CAST")
    fun parseReply(body: ByteArray, httpStatus: Int = 200): OpenWrtResult {
        val root = try {
            Json.parse(String(body, Charsets.UTF_8)) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        }
        if (root == null) {
            return when (httpStatus) {
                404 -> OpenWrtResult(error = "This device has no /ubus endpoint. Is it an OpenWrt router with uhttpd-mod-ubus and rpcd installed?")
                401, 403 -> OpenWrtResult(error = statusMessage(6), accessDenied = true)
                else -> OpenWrtResult(error = "The router did not answer with ubus JSON (HTTP $httpStatus). Is this an OpenWrt router?")
            }
        }
        if (root.containsKey("error")) {
            val error = root.obj("error")
            val code = error.int("code")
            return when (code) {
                -32002 -> OpenWrtResult(status = code, accessDenied = true, error = statusMessage(6))
                -32000 -> OpenWrtResult(status = code, error = "The router does not offer this function (object not found). It may need an extra package; LuCI provides most of them.")
                -32601 -> OpenWrtResult(status = code, error = statusMessage(3))
                -32602 -> OpenWrtResult(status = code, error = statusMessage(2))
                -32003 -> OpenWrtResult(status = code, timedOut = true, error = "The router stopped waiting for an answer. What was asked may still be running on the router.")
                else -> OpenWrtResult(status = code, error = "The router answered: ${error.str("message", "unknown error")}")
            }
        }
        val values = root.list("result")
        val status = (values.firstOrNull() as? Number)?.toInt() ?: return OpenWrtResult(error = "The router's answer had no result.")
        val data = (values.getOrNull(1) as? Map<String, Any?>) ?: emptyMap()
        return if (status == 0) {
            OpenWrtResult(ok = true, status = 0, data = data)
        } else {
            OpenWrtResult(status = status, data = data, error = statusMessage(status), accessDenied = status == OpenWrt.STATUS_PERMISSION_DENIED, timedOut = status == OpenWrt.STATUS_TIMEOUT)
        }
    }

    fun ipv4Bytes(text: String): List<Int>? {
        val parts = text.split(".")
        if (parts.size != 4) return null
        return parts.map { part ->
            if (part.isEmpty() || part.length > 3 || !part.all(Char::isDigit) || (part.length > 1 && part.startsWith("0"))) return null
            part.toInt().takeIf { it <= 255 } ?: return null
        }
    }

    private fun localIpv4(bytes: List<Int>): Boolean {
        val (a, b) = bytes
        return a == 10 || a == 127 || (a == 172 && b in 16..31) || (a == 192 && b == 168) || (a == 169 && b == 254)
    }

    private fun localIpv6(text: String): Boolean? {
        if (!text.contains(":")) return null
        val lower = text.lowercase().substringBefore("%")
        if (lower == "::1") return true
        val value = lower.substringBefore(":").toIntOrNull(16) ?: return false
        return (value and 0xfe00) == 0xfc00 || (value and 0xffc0) == 0xfe80
    }

    fun isLiteralAddress(host: String): Boolean {
        val name = host.trim('[', ']')
        return ipv4Bytes(name) != null || name.contains(":")
    }

    fun isLocalHost(host: String): Boolean {
        val name = host.trim().lowercase().trim('[', ']')
        if (name.isEmpty()) return false
        ipv4Bytes(name)?.let { return localIpv4(it) }
        localIpv6(name)?.let { return it }
        if (name == "localhost" || !name.contains(".")) return true
        return listOf(".lan", ".local", ".home", ".home.arpa", ".internal", ".localdomain", ".router").any { name.endsWith(it) }
    }

    fun parseAddress(input: String): OpenWrtAddress {
        var text = input.trim()
        if (text.isEmpty()) return OpenWrtAddress(error = "Enter the router's address, for example 192.168.1.1.")
        if (!text.contains("://")) text = "http://$text"
        val uri = try {
            URI(text)
        } catch (_: Exception) {
            null
        }
        val scheme = uri?.scheme?.lowercase()
        val host = uri?.host
        if (uri == null || (scheme != "http" && scheme != "https") || host.isNullOrEmpty()) {
            return OpenWrtAddress(error = "\"${input.trim()}\" is not a router address. Use an IP address or a name such as openwrt.lan.")
        }
        if (uri.rawUserInfo != null) return OpenWrtAddress(error = "Leave the user name out of the address; type it in the User field.")
        val bare = host.trim('[', ']')
        if (isLiteralAddress(bare) && !isLocalHost(bare)) {
            return OpenWrtAddress(error = "$bare is not on your local network. Only routers on your own network can be managed, so the router password never crosses the internet.")
        }
        val port = uri.port
        val base = "$scheme://$host" + if (port > 0) ":$port" else ""
        return OpenWrtAddress(base = base, host = bare, port = port, encrypted = scheme == "https")
    }

    fun baseWithHost(address: OpenWrtAddress, host: String): String {
        val scheme = if (address.encrypted) "https" else "http"
        val shown = if (host.contains(":")) "[$host]" else host
        return "$scheme://$shown" + if (address.port > 0) ":${address.port}" else ""
    }

    fun uciSections(values: JsonMap, type: String = ""): List<UciSection> =
        values.entries.mapIndexedNotNull { order, (key, value) ->
            @Suppress("UNCHECKED_CAST")
            val raw = value as? Map<String, Any?> ?: emptyMap()
            val section = UciSection(
                name = raw.str(".name", key),
                type = raw.str(".type"),
                anonymous = raw.bool(".anonymous"),
                index = raw.int(".index"),
                options = raw.filterKeys { !it.startsWith(".") },
            )
            if (type.isNotEmpty() && section.type != type) null else order to section
        }.sortedWith(compareBy({ it.second.index }, { it.first })).map { it.second }

    fun optionText(value: Any?): String = when (value) {
        is List<*> -> value.joinToString(" ") { optionText(it) }
        is Boolean -> if (value) "1" else "0"
        else -> jsonText(value)
    }

    fun optionValue(text: String, list: Boolean): Any =
        if (!list) text.trim() else text.split(' ', ',', '\n', '\t').filter { it.isNotEmpty() }

    private val secretOptions = setOf("key", "key1", "key2", "key3", "key4", "password", "sae_password", "auth_secret", "acct_secret", "private_key", "preshared_key", "psk")

    fun isSecretOption(name: String): Boolean = name.lowercase() in secretOptions

    fun sectionLabel(section: UciSection): String {
        if (!section.anonymous) return section.name
        val name = section.option("name")
        return if (name.isEmpty()) "${section.type} #${section.index}" else "${section.type} \"$name\""
    }

    fun validUciName(name: String): Boolean = name.length in 1..64 && name.all { it.isLetterOrDigit() && it.code < 128 || it == '_' }

    fun pendingCount(changes: JsonMap): Int = changes.values.sumOf { (it as? List<*>)?.size ?: 0 }

    fun describeChanges(changes: JsonMap): List<String> {
        val lines = mutableListOf<String>()
        for ((config, entries) in changes) {
            for (entry in entries as? List<*> ?: emptyList<Any?>()) {
                val change = entry as? List<*> ?: continue
                val op = jsonText(change.getOrNull(0))
                val section = jsonText(change.getOrNull(1))
                val option = jsonText(change.getOrNull(2))
                val shown = if (isSecretOption(option)) "••••••" else optionText(change.getOrNull(3))
                val path = "$config.$section"
                lines += when {
                    op == "set" && change.size > 3 -> "$path.$option = $shown"
                    op == "set" || op == "add" -> "$path: new ${option.ifEmpty { "unnamed" }} section"
                    op == "remove" && option.isNotEmpty() -> "$path.$option removed"
                    op == "remove" -> "$path deleted"
                    op == "list-add" -> "$path.$option += $shown"
                    op == "list-del" -> "$path.$option -= $shown"
                    op == "rename" -> "$path renamed to $option"
                    op == "order" -> "$path moved"
                    else -> "$path: $op"
                }
            }
        }
        return lines
    }

    fun validMac(text: String): Boolean {
        val parts = text.trim().split(":")
        return parts.size == 6 && parts.all { it.length == 2 && it.all { c -> c.isDigit() || c.lowercaseChar() in 'a'..'f' } }
    }

    fun parseLeases(text: String): List<OpenWrtLease> = text.lines().mapNotNull { line ->
        val fields = line.split(' ').filter { it.isNotEmpty() }
        val expires = fields.getOrNull(0)?.toLongOrNull()
        if (fields.size < 4 || expires == null || !validMac(fields[1])) null
        else OpenWrtLease(expires, fields[1].lowercase(), fields[2], if (fields[3] == "*") "" else fields[3])
    }

    fun parseLuciLeases(data: JsonMap, now: Long): List<OpenWrtLease> = data.maps("dhcp_leases").mapNotNull { raw ->
        val mac = raw.str("macaddr").lowercase()
        if (mac.isEmpty()) return@mapNotNull null
        val left = (raw["expires"] as? Number)?.toLong() ?: 0L
        OpenWrtLease(if (left > 0) now + left else 0, mac, raw.str("ipaddr"), raw.str("hostname"))
    }

    fun leaseExpiry(expires: Long, now: Long): String {
        if (expires <= 0) return "Never"
        val left = expires - now
        return if (left <= 0) "Expired" else "in ${formatUptime(left)}"
    }

    fun packagesInstalled(packages: List<OpenWrtPackage>, names: List<String>): Boolean =
        names.all { name -> packages.any { it.name == name } }

    fun parseInstalledPackages(text: String): List<OpenWrtPackage> {
        val packages = mutableListOf<OpenWrtPackage>()
        var blockName = ""
        var blockVersion = ""
        fun flush() {
            if (blockName.isNotEmpty()) packages += OpenWrtPackage(blockName, blockVersion)
            blockName = ""
            blockVersion = ""
        }
        for (raw in text.split("\n")) {
            val line = raw.trim()
            when {
                line.isEmpty() -> flush()
                line.startsWith("Package: ") -> {
                    flush()
                    blockName = line.removePrefix("Package: ").trim()
                }
                line.startsWith("Version: ") -> blockVersion = line.removePrefix("Version: ").trim()
                blockName.isNotEmpty() || line.contains(": ") -> Unit
                line.contains(" - ") -> {
                    val name = line.substringBefore(" - ")
                    val rest = line.substringAfter(" - ")
                    packages += OpenWrtPackage(name, rest.substringBefore(" - "))
                }
                else -> apkPackage(line)?.let { packages += it }
            }
        }
        flush()
        return packages.sortedBy { it.name }
    }

    private fun apkPackage(line: String): OpenWrtPackage? {
        val pieces = line.substringBefore(' ').split("-")
        if (pieces.size < 2) return null
        var versionStart = pieces.size - 1
        val last = pieces[versionStart]
        if (last.startsWith("r") && last.length > 1 && last.drop(1).all(Char::isDigit) && versionStart >= 2) versionStart -= 1
        if (versionStart < 1 || pieces[versionStart].firstOrNull()?.isDigit() != true) return null
        return OpenWrtPackage(pieces.subList(0, versionStart).joinToString("-"), pieces.subList(versionStart, pieces.size).joinToString("-"))
    }

    val packageTools = listOf(
        OpenWrtPackageTool("/usr/libexec/package-manager-call", true),
        OpenWrtPackageTool("/usr/libexec/opkg-call", true),
        OpenWrtPackageTool("/usr/bin/apk", false),
        OpenWrtPackageTool("/bin/opkg", false),
    )

    fun packageArguments(tool: OpenWrtPackageTool, action: String, names: List<String> = emptyList()): List<String> {
        val apk = !tool.wrapper && tool.program.endsWith("/apk")
        return when (action) {
            "list" -> if (tool.wrapper || !apk) listOf("list-installed") else listOf("list", "--installed")
            "update" -> listOf("update")
            "install" -> listOf(if (apk) "add" else "install") + names
            "remove" -> listOf(if (apk) "del" else "remove") + names
            else -> emptyList()
        }
    }

    private val packageNamePattern = Regex("^[a-z0-9][a-z0-9+._-]{0,99}$")

    fun validPackageName(name: String): Boolean = packageNamePattern.matches(name)

    fun parseServices(list: JsonMap): List<OpenWrtService> = list.map { (name, value) ->
        @Suppress("UNCHECKED_CAST")
        val raw = value as? Map<String, Any?> ?: emptyMap()
        OpenWrtService(name, raw.bool("enabled"), raw.bool("running"))
    }.sortedBy { it.name }

    fun parseInterfaces(dump: JsonMap): List<OpenWrtInterfaceStatus> = dump.maps("interface").mapNotNull { raw ->
        val name = raw.str("interface")
        if (name.isEmpty()) return@mapNotNull null
        OpenWrtInterfaceStatus(
            name = name,
            proto = raw.str("proto"),
            device = raw.str("l3_device").ifEmpty { raw.str("device") },
            up = raw.bool("up"),
            uptime = raw.long("uptime"),
            ipv4 = raw.maps("ipv4-address").map { "${it.str("address")}/${it.int("mask")}" },
            ipv6 = raw.maps("ipv6-address").map { "${it.str("address")}/${it.int("mask")}" },
            dns = raw.strings("dns-server"),
            gateway = raw.maps("route").firstOrNull { it.str("target") == "0.0.0.0" && it.int("mask") == 0 }?.str("nexthop") ?: "",
        )
    }

    fun formatUptime(seconds: Long): String {
        val value = maxOf(0L, seconds)
        val days = value / 86400
        val hours = (value % 86400) / 3600
        val minutes = (value % 3600) / 60
        return when {
            days > 0 -> "${days}d ${hours}h"
            hours > 0 -> "${hours}h ${minutes}m"
            minutes > 0 -> "${minutes}m"
            else -> "${value}s"
        }
    }

    fun formatBytes(bytes: Long): String {
        val units = listOf("B", "KB", "MB", "GB", "TB")
        var value = maxOf(0L, bytes).toDouble()
        var unit = 0
        while (value >= 1024 && unit < units.size - 1) {
            value /= 1024
            unit++
        }
        if (unit == 0) return "${value.toLong()} B"
        return String.format(java.util.Locale.ROOT, if (value < 10) "%.1f %s" else "%.0f %s", value, units[unit])
    }

    fun formatLoad(load: List<Any?>): String =
        load.joinToString(" · ") { String.format(java.util.Locale.ROOT, "%.2f", ((it as? Number)?.toDouble() ?: 0.0) / 65536.0) }

    fun validIpv4(text: String): Boolean = ipv4Bytes(text.trim()) != null

    fun validNetmask(text: String): Boolean {
        val bytes = ipv4Bytes(text.trim()) ?: return false
        val mask = bytes.fold(0L) { acc, b -> (acc shl 8) or b.toLong() }
        val inverted = mask.inv() and 0xffffffffL
        return inverted and (inverted + 1) == 0L
    }

    fun validAddressList(text: String, allowPrefix: Boolean): Boolean {
        val parts = text.split(' ', ',').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return false
        return parts.all { part ->
            val pieces = part.split("/")
            if (pieces.size > 2 || !validIpv4(pieces[0])) return@all false
            pieces.size == 1 || (allowPrefix && pieces[1].toIntOrNull()?.let { it in 0..32 } == true)
        }
    }

    fun validPort(text: String): Boolean {
        val parts = text.trim().split('-', ':')
        if (parts.size !in 1..2 || parts.any { it.isEmpty() || it.length > 5 || !it.all(Char::isDigit) }) return false
        val first = parts.first().toInt()
        val last = parts.last().toInt()
        return first >= 1 && last <= 65535 && first <= last
    }

    fun ssidProblem(ssid: String): String? {
        val length = ssid.toByteArray(Charsets.UTF_8).size
        return when {
            length == 0 -> "Give the Wi-Fi network a name."
            length > 32 -> "A Wi-Fi name can be at most 32 bytes long."
            else -> null
        }
    }

    fun wifiKeyProblem(encryption: String, key: String): String? {
        if (encryption.isEmpty() || encryption == "none" || encryption == "owe") return null
        if (encryption.startsWith("psk") && key.length == 64 && key.all { it.isDigit() || it.lowercaseChar() in 'a'..'f' }) return null
        if (key.length < 8) return "The Wi-Fi password needs at least 8 characters."
        if (encryption.startsWith("psk") || encryption == "sae-mixed") {
            if (key.length > 63) return "A WPA2 password can be at most 63 characters."
            if (!key.all { it.code in 0x20..0x7e }) return "A WPA2 password can only use printable ASCII characters."
        }
        return null
    }

    fun hostnameProblem(hostname: String): String? {
        val ok = hostname.length in 1..63 && !hostname.startsWith("-") && !hostname.endsWith("-") &&
            hostname.all { it.code < 128 && (it.isLetterOrDigit() || it == '-') }
        return if (ok) null else "A hostname uses letters, digits and hyphens, at most 63 characters, and cannot start or end with a hyphen."
    }

    fun passwordProblem(password: String, confirm: String): String? = when {
        password.length < 8 -> "Use at least 8 characters for the router password."
        password != confirm -> "The two passwords do not match."
        else -> null
    }

    val encryptionChoices = listOf(
        OpenWrtChoice("sae-mixed", "WPA2/WPA3 (recommended)"),
        OpenWrtChoice("sae", "WPA3 only"),
        OpenWrtChoice("psk2", "WPA2"),
        OpenWrtChoice("psk-mixed", "WPA/WPA2 (older devices)"),
        OpenWrtChoice("owe", "Open, encrypted (OWE)"),
        OpenWrtChoice("none", "Open, no password"),
    )

    val protocolChoices = listOf(
        OpenWrtChoice("static", "Static address"),
        OpenWrtChoice("dhcp", "DHCP client"),
        OpenWrtChoice("dhcpv6", "DHCPv6 client"),
        OpenWrtChoice("pppoe", "PPPoE"),
        OpenWrtChoice("none", "Unmanaged"),
    )

    val forwardProtocolChoices = listOf(
        OpenWrtChoice("tcp udp", "TCP and UDP"),
        OpenWrtChoice("tcp", "TCP"),
        OpenWrtChoice("udp", "UDP"),
    )

    fun choiceLabel(choices: List<OpenWrtChoice>, value: String): String = choices.firstOrNull { it.value == value }?.label ?: value

    fun certificateFingerprint(der: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(der).joinToString(":") { "%02X".format(it.toInt() and 0xff) }

    fun wifiBand(radio: UciSection): String = when (radio.option("band")) {
        "2g" -> "2.4 GHz"
        "5g" -> "5 GHz"
        "6g" -> "6 GHz"
        "60g" -> "60 GHz"
        else -> when (radio.option("hwmode")) {
            "11a" -> "5 GHz"
            "" -> "Radio"
            else -> "2.4 GHz"
        }
    }

    fun sanitizedHostname(name: String, mac: String): String {
        val clean = name.map { if (it.code < 128 && (it.isLetterOrDigit() || it == '-')) it else '-' }.joinToString("").take(63).trim('-')
        return clean.ifEmpty { "device-" + mac.replace(":", "").takeLast(6).lowercase() }
    }

    fun interfaceEdit(
        section: UciSection,
        proto: String,
        ipaddr: String,
        netmask: String,
        gateway: String,
        dns: String,
        pppoeUser: String,
        pppoePassword: String,
    ): UciEdit {
        val values = linkedMapOf<String, Any?>("proto" to proto)
        val removed = mutableListOf<String>()
        val address = ipaddr.trim()
        if (proto == "static") {
            if (!validAddressList(address, true)) throw OpenWrtProblem("Enter a valid IPv4 address, for example 192.168.1.1.")
            val cidr = address.contains("/")
            if (!cidr && !validNetmask(netmask)) throw OpenWrtProblem("Enter a valid netmask, for example 255.255.255.0.")
            val gw = gateway.trim()
            if (gw.isNotEmpty() && !validIpv4(gw)) throw OpenWrtProblem("The gateway must be an IPv4 address.")
            values["ipaddr"] = optionValue(address, section.isList("ipaddr") || address.contains(' ') || address.contains(','))
            if (cidr) removed += "netmask" else values["netmask"] = netmask.trim()
            if (gw.isEmpty()) removed += "gateway" else values["gateway"] = gw
        } else {
            removed += listOf("ipaddr", "netmask", "gateway")
        }
        if (proto == "pppoe") {
            val user = pppoeUser.trim()
            if (user.isEmpty()) throw OpenWrtProblem("PPPoE needs the user name from your internet provider.")
            values["username"] = user
            if (pppoePassword.isNotEmpty()) values["password"] = pppoePassword
        } else {
            removed += listOf("username", "password")
        }
        val servers = dns.trim()
        if (servers.isEmpty() || proto == "none") {
            removed += "dns"
        } else {
            val parts = servers.split(' ', ',').filter { it.isNotEmpty() }
            if (!parts.all { validIpv4(it) || it.contains(":") }) throw OpenWrtProblem("DNS servers must be IP addresses separated by spaces.")
            values["dns"] = parts
        }
        return UciEdit(values, removed.filter { section.options.containsKey(it) })
    }

    fun dhcpEdit(enabled: Boolean, start: Int, limit: Int, leaseTime: String): UciEdit {
        if (!enabled) return UciEdit(mapOf("ignore" to "1"))
        return UciEdit(
            linkedMapOf("start" to start.toString(), "limit" to limit.toString(), "leasetime" to leaseTime.trim().ifEmpty { "12h" }),
            listOf("ignore"),
        )
    }

    fun radioEdit(radio: UciSection, enabled: Boolean, channel: String, country: String, txpower: String): UciEdit {
        val channelText = channel.trim().lowercase()
        if (channelText.isNotEmpty() && channelText != "auto" && channelText.toIntOrNull()?.let { it in 1..233 } != true) {
            throw OpenWrtProblem("The channel is \"auto\" or a channel number.")
        }
        val code = country.trim().uppercase()
        if (code.isNotEmpty() && (code.length != 2 || !code.all { it.code < 128 && it.isLetterOrDigit() })) {
            throw OpenWrtProblem("The country is a two-letter code such as US or DE.")
        }
        val power = txpower.trim()
        if (power.isNotEmpty() && power.toIntOrNull()?.let { it in 1..36 } != true) {
            throw OpenWrtProblem("Transmit power is a number of dBm between 1 and 36, or empty for the driver default.")
        }
        val values = linkedMapOf<String, Any?>("channel" to channelText.ifEmpty { "auto" })
        val removed = mutableListOf<String>()
        if (enabled) removed += "disabled" else values["disabled"] = "1"
        if (code.isEmpty()) removed += "country" else values["country"] = code
        if (power.isEmpty()) removed += "txpower" else values["txpower"] = power
        return UciEdit(values, removed.filter { radio.options.containsKey(it) })
    }

    fun wifiEdit(iface: UciSection, ssid: String, encryption: String, key: String, network: String, enabled: Boolean, hidden: Boolean, isolate: Boolean): UciEdit {
        (ssidProblem(ssid) ?: wifiKeyProblem(encryption, key))?.let { throw OpenWrtProblem(it) }
        val values = linkedMapOf<String, Any?>("ssid" to ssid, "encryption" to encryption)
        val removed = mutableListOf<String>()
        if (encryption == "none" || encryption == "owe") removed += "key" else values["key"] = key
        if (network.isNotEmpty()) values["network"] = network
        if (enabled) removed += "disabled" else values["disabled"] = "1"
        if (hidden) values["hidden"] = "1" else removed += "hidden"
        if (isolate) values["isolate"] = "1" else removed += "isolate"
        return UciEdit(values, removed.filter { iface.options.containsKey(it) })
    }

    fun newWifiValues(radio: String, network: String, ssid: String, key: String): Map<String, Any?> {
        (ssidProblem(ssid) ?: wifiKeyProblem("sae-mixed", key))?.let { throw OpenWrtProblem(it) }
        return linkedMapOf("device" to radio, "mode" to "ap", "network" to network, "ssid" to ssid, "encryption" to "sae-mixed", "key" to key)
    }

    fun forwardEdit(existing: UciSection?, name: String, proto: String, externalPort: String, ip: String, internalPort: String, enabled: Boolean): UciEdit {
        val label = name.trim()
        val external = externalPort.trim()
        val address = ip.trim()
        val target = internalPort.trim()
        if (label.isEmpty()) throw OpenWrtProblem("Give the port forward a name.")
        if (!validPort(external)) throw OpenWrtProblem("The external port is a number from 1 to 65535, or a range such as 6000-6010.")
        if (!validIpv4(address)) throw OpenWrtProblem("Enter the IPv4 address of the device on your network.")
        if (target.isNotEmpty() && !validPort(target)) throw OpenWrtProblem("The port on the device is a number from 1 to 65535, or empty to use the external port.")
        val values = linkedMapOf<String, Any?>("name" to label, "proto" to optionValue(proto, true), "src_dport" to external, "dest_ip" to address)
        val removed = mutableListOf<String>()
        if (target.isEmpty()) removed += "dest_port" else values["dest_port"] = target
        if (enabled) removed += "enabled" else values["enabled"] = "0"
        if (existing == null) {
            values["target"] = "DNAT"
            values["src"] = "wan"
            values["dest"] = "lan"
            return UciEdit(values, emptyList())
        }
        return UciEdit(values, removed.filter { existing.options.containsKey(it) })
    }

    fun reservationValues(name: String, mac: String, ip: String): Map<String, Any?> {
        val address = mac.trim().lowercase()
        val ipText = ip.trim()
        val label = name.trim()
        if (!validMac(address)) throw OpenWrtProblem("Enter the device's MAC address, for example aa:bb:cc:dd:ee:ff.")
        if (!validIpv4(ipText)) throw OpenWrtProblem("Enter the IPv4 address to reserve.")
        if (label.isNotEmpty()) hostnameProblem(label)?.let { throw OpenWrtProblem(it) }
        return linkedMapOf("name" to sanitizedHostname(label, address), "mac" to address, "ip" to ipText, "dns" to "1")
    }

    private val storagePattern = Regex("^(sd[a-z]+\\d*|mmcblk\\d+(p\\d+)?|nvme\\d+n\\d+(p\\d+)?|hd[a-z]+\\d*|vd[a-z]+\\d*)$")

    fun isStorageDevice(name: String): Boolean = storagePattern.matches(name)

    fun parseBlockDevices(devices: JsonMap): List<OpenWrtBlockDevice> = devices.mapNotNull { (key, value) ->
        @Suppress("UNCHECKED_CAST")
        val raw = value as? Map<String, Any?> ?: emptyMap()
        val device = raw.str("dev", "/dev/$key")
        val name = device.substringAfterLast('/')
        if (!isStorageDevice(name)) return@mapNotNull null
        OpenWrtBlockDevice(name, device, raw.str("uuid"), raw.str("label"), raw.str("type"), raw.str("mount"), jsonLong(raw["size"]))
    }.sortedBy { it.name }

    fun parseMountPoints(data: JsonMap): List<OpenWrtMountPoint> = data.maps("result").mapNotNull { raw ->
        val mount = raw.str("mount")
        if (mount.isEmpty()) null else OpenWrtMountPoint(raw.str("device"), mount, raw.str("type"), raw.long("size"), raw.long("avail"))
    }

    fun storageDisks(blocks: List<OpenWrtBlockDevice>, mounts: List<OpenWrtMountPoint>, fstab: List<UciSection>): List<OpenWrtDisk> = blocks.map { block ->
        var disk = OpenWrtDisk(block, mount = block.mount)
        mounts.firstOrNull { it.device == block.device || (disk.mount.isNotEmpty() && it.mount == disk.mount) }?.let {
            disk = disk.copy(mount = it.mount, size = it.size, avail = it.avail, usageKnown = it.size > 0)
        }
        if (!disk.usageKnown) disk = disk.copy(size = block.size)
        fstab.firstOrNull { section ->
            val uuid = section.option("uuid")
            val device = section.option("device")
            val label = section.option("label")
            section.type == "mount" && ((uuid.isNotEmpty() && uuid.equals(block.uuid, ignoreCase = true)) ||
                (device.isNotEmpty() && device == block.device) || (label.isNotEmpty() && label == block.label))
        }?.let { disk = disk.copy(fstabSection = it.name, bootTarget = it.option("target"), bootEnabled = it.flag("enabled", true)) }
        disk
    }

    private val mountPathPattern = Regex("^/(mnt|media|srv|data)(/[A-Za-z0-9._-]+)+$")

    fun mountPathProblem(path: String): String? {
        val bad = !mountPathPattern.matches(path) || path.split("/").any { it == "." || it == ".." }
        return if (bad) "Use a folder under /mnt, /media, /srv or /data, for example /mnt/usb, with letters, digits, dots, dashes and underscores." else null
    }

    fun suggestedMountPath(block: OpenWrtBlockDevice): String {
        val name = block.label.map { if (it.code < 128 && (it.isLetterOrDigit() || it in "._-")) it else '_' }.joinToString("").trimStart('.')
        return "/mnt/" + name.ifEmpty { block.name }
    }

    val networkSharePackages = listOf("ksmbd-server", "luci-app-ksmbd")

    val usbStoragePackages = listOf("kmod-usb-storage", "kmod-usb-storage-uas", "block-mount", "kmod-fs-ext4", "kmod-fs-vfat", "kmod-fs-exfat")

    const val FSTAB_PATH = "/etc/config/fstab"
    const val DEFAULT_FSTAB = "config global\n\toption anon_swap '0'\n\toption anon_mount '0'\n\toption auto_swap '1'\n\toption auto_mount '1'\n\toption delay_root '5'\n\toption check_fs '0'\n"

    fun parsePartitions(text: String): List<String> = text.lines()
        .map { it.trim().split(Regex("\\s+")) }
        .filter { it.size == 4 && isStorageDevice(it[3]) }
        .map { it[3] }
        .distinct()

    private val usbControllerPattern = Regex("^usb\\d+$")
    private val usbDevicePattern = Regex("^\\d+-\\d+(\\.\\d+)*$")

    fun usbControllerCount(entries: List<String>): Int = entries.count { usbControllerPattern.matches(it) }

    fun usbDeviceEntries(entries: List<String>): List<String> = entries.filter { usbDevicePattern.matches(it) }.sorted()

    private val kernelPriority = Regex("^<\\d+>")
    private val kernelWanted = Regex("usb|uas|scsi|xhci|ehci|ohci|\\bsd[a-z]+\\d*\\b|exfat|ext4|vfat|fat-fs|mmc\\d|i/o error", RegexOption.IGNORE_CASE)

    fun storageKernelLines(dmesg: String, limit: Int = 20): List<String> = dmesg.lines()
        .map { it.replace(kernelPriority, "").trim() }
        .filter { it.isNotEmpty() && kernelWanted.containsMatchIn(it) }
        .takeLast(limit)

    fun missingStoragePackages(installed: List<String>): List<String> = usbStoragePackages.filter { it !in installed }

    private val fstabGlobal = Regex("^config\\s+'?global'?\\s*$", RegexOption.MULTILINE)

    fun fstabFromDetect(output: String): String = when {
        !fstabGlobal.containsMatchIn(output) -> DEFAULT_FSTAB
        output.endsWith("\n") -> output
        else -> output + "\n"
    }

    fun outputTail(text: String, count: Int = 4): List<String> = text.lines().map { it.trim() }.filter { it.isNotEmpty() }.takeLast(count)

    fun storageFindings(check: OpenWrtStorageCheck): List<String> {
        val findings = mutableListOf<String>()
        val devices = check.usbDevices.joinToString(", ")
        val partitions = check.partitions.joinToString(", ")
        val missing = missingStoragePackages(check.installed)
        if (check.listProblem.isNotEmpty()) findings += "The router could not list its disks: ${check.listProblem}"
        if (check.usbKnown && check.usbControllers == 0 && check.partitions.isEmpty()) {
            findings += "The router has no USB controller driver running, so it cannot see anything in its USB port. Install the driver for this router (usually kmod-usb2 or kmod-usb3), or check that the model has a USB port."
        } else if (check.usbKnown && check.usbDevices.isEmpty() && check.partitions.isEmpty()) {
            findings += "The router sees nothing plugged into its USB port. Push the drive in fully, try another port or cable, and give a hard disk that needs more power a powered USB hub."
        } else if (check.partitionsKnown && check.partitions.isEmpty() && check.usbDevices.isNotEmpty()) {
            findings += if (check.packagesKnown && "kmod-usb-storage" !in check.installed) {
                "A USB device is plugged in ($devices), but the USB storage driver (kmod-usb-storage) is not installed, so no disk appears. Press Install USB storage support."
            } else {
                "A USB device is plugged in ($devices), but no disk appeared for it. Unplug it and plug it back in, or restart the router; the kernel messages below may say why."
            }
        }
        if (check.partitions.isNotEmpty() && !check.blockTool) {
            findings += "The router sees the disk ($partitions), but block-mount is not installed, so it cannot list or mount it. Press Install USB storage support."
        } else if (check.partitions.isNotEmpty() && check.disks == 0 && check.listProblem.isEmpty()) {
            val drivers = missing.filter { it.startsWith("kmod-fs-") }
            var text = "The router sees the disk ($partitions), but found no filesystem it can read on it. Format it on a computer as ext4, exFAT or FAT32."
            if (check.packagesKnown && drivers.isNotEmpty()) text += " These filesystem drivers are not installed: ${drivers.joinToString(", ")}."
            findings += text
        }
        if (check.blockTool && !check.fstabExists) {
            findings += "The router has no storage settings file (/etc/config/fstab) yet. Turning on Mount any disk when it is plugged in creates it."
        }
        if (!check.packagesKnown) {
            findings += "ForkMesh cannot run the router's package manager, so it cannot tell which drivers are installed."
        } else if (missing.isNotEmpty()) {
            findings += "Not installed: ${missing.joinToString(", ")}."
        }
        val unmounted = check.disks - check.mounted
        if (unmounted > 0) {
            val count = if (unmounted == 1) "1 disk is not mounted." else "$unmounted disks are not mounted."
            findings += count + when {
                !check.autoMount -> " Turn on Mount any disk when it is plugged in and apply, or choose Mount at boot for the disk; then press Mount disks now."
                check.autoMountPending -> " Apply your changes, then press Mount disks now."
                else -> " Press Mount disks now; if it stays unmounted, its filesystem driver may be missing."
            }
        } else if (check.disks > 0) {
            findings += if (check.disks == 1) "The disk is mounted." else "All ${check.disks} disks are mounted."
        }
        if (findings.isEmpty()) findings += "No problem found."
        return findings
    }

    fun storageReport(check: OpenWrtStorageCheck): String {
        val lines = storageFindings(check).map { "• $it" }.toMutableList()
        lines += ""
        if (check.usbKnown) {
            lines += "USB controllers: ${check.usbControllers}"
            lines += "USB devices: ${if (check.usbDevices.isEmpty()) "none" else check.usbDevices.joinToString(", ")}"
        } else {
            lines += "USB devices: the router did not list them"
        }
        lines += "Disks the kernel sees: " + when {
            !check.partitionsKnown -> "could not read /proc/partitions"
            check.partitions.isEmpty() -> "none"
            else -> check.partitions.joinToString(", ")
        }
        lines += "Disks with a readable filesystem: ${check.disks}, mounted: ${check.mounted}"
        lines += "block-mount: ${if (check.blockTool) "installed" else "not installed"}"
        lines += "Storage settings (/etc/config/fstab): ${if (check.fstabExists) "present" else "missing"}"
        lines += "Mount any disk when it is plugged in: ${if (check.autoMount) "on" else "off"}${if (check.autoMountPending) ", waiting to be applied" else ""}"
        val missing = missingStoragePackages(check.installed)
        lines += when {
            !check.packagesKnown -> "Storage packages: unknown"
            missing.isEmpty() -> "Storage packages: all installed"
            else -> "Storage packages missing: ${missing.joinToString(", ")}"
        }
        if (check.kernelLines.isEmpty()) {
            lines += "Recent kernel messages: none about USB or disks"
        } else {
            lines += "Recent kernel messages:"
            lines += check.kernelLines.map { "  $it" }
        }
        return lines.joinToString("\n")
    }

    fun shareConfig(configs: List<String>): String = when {
        "ksmbd" in configs -> "ksmbd"
        "samba4" in configs -> "samba4"
        else -> ""
    }

    fun shareSectionType(config: String): String = when (config) {
        "ksmbd" -> "share"
        "samba4" -> "sambashare"
        else -> ""
    }

    fun parseShares(sections: List<UciSection>): List<OpenWrtShare> = sections.filter { it.type == "share" || it.type == "sambashare" }.map { section ->
        fun yes(key: String) = section.option(key).lowercase() in setOf("yes", "1", "true")
        OpenWrtShare(section.name, section.option("name", section.name), section.option("path"), yes("read_only"), yes("guest_ok"))
    }

    private val shareNamePattern = Regex("^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$")

    fun shareNameProblem(name: String): String? =
        if (shareNamePattern.matches(name)) null else "A share name uses up to 32 letters, digits, dots, dashes and underscores."

    fun shareValues(name: String, path: String, guests: Boolean): Map<String, Any?> = linkedMapOf(
        "name" to name, "path" to path, "read_only" to "no", "guest_ok" to if (guests) "yes" else "no", "create_mask" to "0666", "dir_mask" to "0777",
    )

    fun mountSectionValues(block: OpenWrtBlockDevice, target: String): Map<String, Any?> {
        val values = linkedMapOf<String, Any?>("target" to target, "enabled" to "1")
        if (block.uuid.isEmpty()) values["device"] = block.device else values["uuid"] = block.uuid
        return values
    }
}
