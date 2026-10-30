package com.forkmesh.nativeapp.nexus

private fun String.allDigits(): Boolean = isNotEmpty() && all { it in '0'..'9' }

data class IPv4Addr(val value: Long) : Comparable<IPv4Addr> {
    init {
        require(value in 0L..0xFFFF_FFFFL)
    }

    constructor(a: Int, b: Int, c: Int, d: Int) : this((a.toLong() shl 24) or (b.toLong() shl 16) or (c.toLong() shl 8) or d.toLong())

    val octets: List<Int> get() = listOf((value shr 24).toInt() and 0xFF, (value shr 16).toInt() and 0xFF, (value shr 8).toInt() and 0xFF, value.toInt() and 0xFF)

    override fun toString(): String = octets.joinToString(".")

    val isUnspecified: Boolean get() = value == 0L
    val isLoopback: Boolean get() = value shr 24 == 127L
    val isPrivate: Boolean get() = value shr 24 == 10L || value shr 20 == 0xAC1L || value shr 16 == 0xC0A8L
    val isLinkLocal: Boolean get() = value shr 16 == 0xA9FEL
    val isSharedAddressSpace: Boolean get() = value shr 22 == (100L shl 2 or 1L)
    val isMulticast: Boolean get() = value shr 28 == 0xEL
    val isBroadcast: Boolean get() = value == 0xFFFF_FFFFL
    val isDocumentation: Boolean get() = value shr 8 == 0xC00002L || value shr 8 == 0xC63364L || value shr 8 == 0xCB0071L
    val isLocalScope: Boolean get() = isLoopback || isPrivate || isLinkLocal || isSharedAddressSpace

    val reversePointerName: String get() = octets.reversed().joinToString(".") + ".in-addr.arpa"

    override fun compareTo(other: IPv4Addr): Int = value.compareTo(other.value)

    companion object {
        fun of(bytes: ByteArray): IPv4Addr? {
            if (bytes.size != 4) return null
            return IPv4Addr(bytes.fold(0L) { acc, b -> acc shl 8 or (b.toLong() and 0xFF) })
        }

        fun parse(text: String): IPv4Addr? {
            val parts = text.split(".")
            if (parts.size != 4) return null
            var result = 0L
            for (part in parts) {
                if (!part.allDigits() || part.length > 3) return null
                if (part.length > 1 && part[0] == '0') return null
                val octet = part.toInt()
                if (octet > 255) return null
                result = result shl 8 or octet.toLong()
            }
            return IPv4Addr(result)
        }
    }
}

class IPv6Addr private constructor(val bytes: List<Int>, val zone: String?) : Comparable<IPv6Addr> {
    val groups: List<Int> get() = (0 until 16 step 2).map { bytes[it] shl 8 or bytes[it + 1] }

    val ipv4Mapped: IPv4Addr?
        get() {
            if (bytes.subList(0, 10).any { it != 0 } || bytes[10] != 0xFF || bytes[11] != 0xFF) return null
            return IPv4Addr.of(ByteArray(4) { bytes[12 + it].toByte() })
        }

    override fun toString(): String {
        val suffix = zone?.let { "%$it" } ?: ""
        ipv4Mapped?.let { return "::ffff:$it$suffix" }
        val groups = groups
        var bestStart = -1
        var bestLength = 0
        var index = 0
        while (index < 8) {
            if (groups[index] != 0) {
                index++
                continue
            }
            var end = index
            while (end < 8 && groups[end] == 0) end++
            if (end - index > bestLength) {
                bestStart = index
                bestLength = end - index
            }
            index = end
        }
        val hex = groups.map { it.toString(16) }
        if (bestLength < 2) return hex.joinToString(":") + suffix
        return hex.subList(0, bestStart).joinToString(":") + "::" + hex.subList(bestStart + bestLength, 8).joinToString(":") + suffix
    }

    val isUnspecified: Boolean get() = bytes.all { it == 0 }
    val isLoopback: Boolean get() = bytes.subList(0, 15).all { it == 0 } && bytes[15] == 1
    val isLinkLocal: Boolean get() = bytes[0] == 0xFE && bytes[1] and 0xC0 == 0x80
    val isUniqueLocal: Boolean get() = bytes[0] and 0xFE == 0xFC
    val isMulticast: Boolean get() = bytes[0] == 0xFF
    val isDocumentation: Boolean get() = bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0D && bytes[3] == 0xB8

    val isLocalScope: Boolean
        get() = ipv4Mapped?.isLocalScope ?: (isLoopback || isLinkLocal || isUniqueLocal)

    val reversePointerName: String
        get() = bytes.reversed().flatMap { listOf((it and 0x0F).toString(16), (it shr 4).toString(16)) }.joinToString(".") + ".ip6.arpa"

    fun withoutZone(): IPv6Addr = IPv6Addr(bytes, null)

    override fun compareTo(other: IPv6Addr): Int {
        for (i in 0 until 16) {
            val diff = bytes[i] - other.bytes[i]
            if (diff != 0) return diff
        }
        return 0
    }

    override fun equals(other: Any?): Boolean = other is IPv6Addr && bytes == other.bytes && zone == other.zone
    override fun hashCode(): Int = bytes.hashCode() * 31 + (zone?.hashCode() ?: 0)

    companion object {
        fun of(bytes: ByteArray, zone: String? = null): IPv6Addr? = of(bytes.map { it.toInt() and 0xFF }, zone)

        fun of(bytes: List<Int>, zone: String? = null): IPv6Addr? {
            if (bytes.size != 16 || bytes.any { it !in 0..255 }) return null
            return IPv6Addr(bytes.toList(), zone?.takeIf { it.isNotEmpty() })
        }

        fun parse(input: String): IPv6Addr? {
            var text = input
            if (text.startsWith("[") && text.endsWith("]")) text = text.substring(1, text.length - 1)
            var zone: String? = null
            val percent = text.indexOf('%')
            if (percent >= 0) {
                zone = text.substring(percent + 1)
                text = text.substring(0, percent)
                if (zone.isEmpty()) return null
            }
            if (':' !in text || text.length > 45) return null
            var tail = emptyList<Int>()
            val lastColon = text.lastIndexOf(':')
            if ('.' in text.substring(lastColon + 1)) {
                val v4 = IPv4Addr.parse(text.substring(lastColon + 1)) ?: return null
                tail = v4.octets
                text = text.substring(0, lastColon + 1)
                if (!text.endsWith("::")) text = text.dropLast(1)
            }
            val halves = text.split("::")
            if (halves.size > 2) return null
            fun groups(part: String): List<Int>? {
                if (part.isEmpty()) return emptyList()
                return part.split(":").map { group ->
                    if (group.isEmpty() || group.length > 4 || !group.all { it in '0'..'9' || it.lowercaseChar() in 'a'..'f' }) return null
                    group.toInt(16)
                }
            }
            val head = groups(halves[0]) ?: return null
            val rest = if (halves.size == 2) groups(halves[1]) ?: return null else emptyList()
            val used = head.size + rest.size + tail.size / 2
            val all = if (halves.size == 2) {
                if (used > 7) return null
                head + List(8 - used) { 0 } + rest
            } else {
                if (used != 8) return null
                head
            }
            val result = all.flatMap { listOf(it shr 8, it and 0xFF) } + tail
            if (result.size != 16) return null
            return IPv6Addr(result, zone)
        }
    }
}

sealed class IPAddr : Comparable<IPAddr> {
    data class V4(val address: IPv4Addr) : IPAddr() {
        override fun toString(): String = address.toString()
    }

    data class V6(val address: IPv6Addr) : IPAddr() {
        override fun toString(): String = address.toString()
    }

    enum class Family { IPv4, IPv6 }

    val family: Family get() = if (this is V4) Family.IPv4 else Family.IPv6

    val urlHost: String
        get() = when (this) {
            is V4 -> address.toString()
            is V6 -> "[${address.withoutZone()}]"
        }

    val isLocalScope: Boolean
        get() = when (this) {
            is V4 -> address.isLocalScope
            is V6 -> address.isLocalScope
        }

    val isLoopback: Boolean
        get() = when (this) {
            is V4 -> address.isLoopback
            is V6 -> address.isLoopback || address.ipv4Mapped?.isLoopback == true
        }

    val isMulticastOrBroadcast: Boolean
        get() = when (this) {
            is V4 -> address.isMulticast || address.isBroadcast
            is V6 -> address.isMulticast
        }

    val isUnspecified: Boolean
        get() = when (this) {
            is V4 -> address.isUnspecified
            is V6 -> address.isUnspecified
        }

    val reversePointerName: String
        get() = when (this) {
            is V4 -> address.reversePointerName
            is V6 -> address.ipv4Mapped?.reversePointerName ?: address.reversePointerName
        }

    override fun compareTo(other: IPAddr): Int = when {
        this is V4 && other is V4 -> address.compareTo(other.address)
        this is V6 && other is V6 -> address.compareTo(other.address)
        this is V4 -> -1
        else -> 1
    }

    companion object {
        fun parse(text: String): IPAddr? = IPv4Addr.parse(text)?.let(::V4) ?: IPv6Addr.parse(text)?.let(::V6)
    }
}

data class IPv4Network private constructor(val network: IPv4Addr, val prefixLength: Int) {
    val netmask: IPv4Addr get() = IPv4Addr(mask(prefixLength))
    val broadcast: IPv4Addr get() = IPv4Addr(network.value or (mask(prefixLength).inv() and 0xFFFF_FFFFL))
    val addressCount: Long get() = 1L shl (32 - prefixLength)
    val hostCount: Long get() = if (prefixLength >= 31) addressCount else addressCount - 2
    val firstHost: IPv4Addr get() = if (prefixLength >= 31) network else IPv4Addr(network.value + 1)
    val lastHost: IPv4Addr get() = if (prefixLength >= 31) broadcast else IPv4Addr(broadcast.value - 1)

    fun contains(address: IPv4Addr): Boolean = address.value and mask(prefixLength) == network.value

    fun hosts(limit: Int = Int.MAX_VALUE): List<IPv4Addr> {
        val count = minOf(hostCount, limit.toLong()).toInt()
        if (count <= 0) return emptyList()
        return (0 until count).map { IPv4Addr(firstHost.value + it) }
    }

    override fun toString(): String = "$network/$prefixLength"

    companion object {
        fun mask(length: Int): Long = if (length == 0) 0L else (0xFFFF_FFFFL shl (32 - length)) and 0xFFFF_FFFFL

        fun of(address: IPv4Addr, prefixLength: Int): IPv4Network? {
            if (prefixLength !in 0..32) return null
            return IPv4Network(IPv4Addr(address.value and mask(prefixLength)), prefixLength)
        }

        fun of(address: IPv4Addr, netmask: IPv4Addr): IPv4Network? {
            val inverted = netmask.value.inv() and 0xFFFF_FFFFL
            val length = if (inverted == 0L) 32 else java.lang.Long.numberOfLeadingZeros(inverted) - 32
            if (mask(length) != netmask.value) return null
            return of(address, length)
        }

        fun parse(text: String): IPv4Network? {
            val parts = text.split("/")
            if (parts.size != 2 || !parts[1].allDigits() || parts[1].length > 3) return null
            val address = IPv4Addr.parse(parts[0]) ?: return null
            return of(address, parts[1].toInt())
        }
    }
}

data class IPv6Network private constructor(val network: IPv6Addr, val prefixLength: Int) {
    fun contains(address: IPv6Addr): Boolean = of(address, prefixLength)?.network == network

    val addressCount: Long? get() = if (prefixLength >= 65) 1L shl (128 - prefixLength) else null

    fun addresses(limit: Int): List<IPv6Addr> {
        val total = addressCount ?: return emptyList()
        val count = minOf(total, limit.toLong()).toInt()
        return (0 until count).map { offset ->
            val bytes = network.bytes.toMutableList()
            var carry = offset.toLong()
            var index = 15
            while (carry > 0 && index >= 0) {
                val sum = bytes[index] + (carry and 0xFF)
                bytes[index] = (sum and 0xFF).toInt()
                carry = (carry shr 8) + (sum shr 8)
                index--
            }
            IPv6Addr.of(bytes)!!
        }
    }

    override fun toString(): String = "$network/$prefixLength"

    companion object {
        fun of(address: IPv6Addr, prefixLength: Int): IPv6Network? {
            if (prefixLength !in 0..128) return null
            val bytes = address.bytes.mapIndexed { index, byte ->
                val before = index * 8
                when {
                    before >= prefixLength -> 0
                    before + 8 > prefixLength -> byte and ((0xFF shl (8 - (prefixLength - before))) and 0xFF)
                    else -> byte
                }
            }
            return IPv6Network(IPv6Addr.of(bytes)!!, prefixLength)
        }

        fun parse(text: String): IPv6Network? {
            val parts = text.split("/")
            if (parts.size != 2 || !parts[1].allDigits() || parts[1].length > 3) return null
            val address = IPv6Addr.parse(parts[0]) ?: return null
            return of(address, parts[1].toInt())
        }
    }
}

sealed class IPNetwork {
    data class V4(val network: IPv4Network) : IPNetwork() {
        override fun toString(): String = network.toString()
    }

    data class V6(val network: IPv6Network) : IPNetwork() {
        override fun toString(): String = network.toString()
    }

    fun contains(address: IPAddr): Boolean = when {
        this is V4 && address is IPAddr.V4 -> network.contains(address.address)
        this is V6 && address is IPAddr.V6 -> network.contains(address.address)
        this is V4 && address is IPAddr.V6 -> address.address.ipv4Mapped?.let(network::contains) ?: false
        else -> false
    }

    companion object {
        fun parse(text: String): IPNetwork? = IPv4Network.parse(text)?.let(::V4) ?: IPv6Network.parse(text)?.let(::V6)
    }
}

object Hostname {
    fun normalized(text: String): String? {
        var name = text.lowercase()
        if (name.endsWith(".")) name = name.dropLast(1)
        if (name.isEmpty() || name.toByteArray().size > 253) return null
        val labels = name.split(".")
        for (label in labels) {
            if (label.isEmpty() || label.length > 63) return null
            if (label.first() == '-' || label.last() == '-') return null
            if (!label.all { it in 'a'..'z' || it in '0'..'9' || it == '-' || it == '_' }) return null
        }
        if (labels.size > 1 && labels.last().allDigits()) return null
        if (labels.all { it.allDigits() }) return null
        return name
    }

    fun isLocal(name: String): Boolean {
        val lower = name.lowercase()
        return lower == "localhost" || lower.endsWith(".local") || lower.endsWith(".localhost") ||
            lower.endsWith(".home.arpa") || lower.endsWith(".lan") || lower.endsWith(".internal") || '.' !in lower
    }
}

data class InterfaceAddress(val name: String, val address: IPv4Addr, val netmask: IPv4Addr)

object LocalNetwork {
    val skippedInterfacePrefixes = listOf("rmnet", "ccmni", "pdp_ip", "tun", "utun", "ppp", "ipsec", "p2p", "aware", "awdl", "llw", "dummy", "docker", "veth", "virbr", "br-")
    val preferredInterfaces = listOf("wlan0", "en0", "eth0")

    fun current(): IPv4Network? = suggested(interfaceAddresses())

    fun suggested(interfaces: List<InterfaceAddress>): IPv4Network? {
        val candidates = interfaces.filter { entry ->
            !entry.address.isLoopback && !entry.address.isLinkLocal && entry.address.isLocalScope &&
                skippedInterfacePrefixes.none { entry.name.startsWith(it) }
        }
        val entry = preferredInterfaces.firstNotNullOfOrNull { name -> candidates.firstOrNull { it.name == name } } ?: candidates.firstOrNull() ?: return null
        val prefix = IPv4Network.of(entry.address, entry.netmask)?.prefixLength ?: 24
        return IPv4Network.of(entry.address, prefix.coerceIn(24, 32))
    }

    fun interfaceAddresses(): List<InterfaceAddress> = runCatching {
        java.net.NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
            .filter { runCatching { it.isUp && !it.isLoopback }.getOrDefault(false) }
            .flatMap { face ->
                face.interfaceAddresses.mapNotNull { entry ->
                    val raw = entry.address as? java.net.Inet4Address ?: return@mapNotNull null
                    val address = IPv4Addr.of(raw.address) ?: return@mapNotNull null
                    val prefix = entry.networkPrefixLength.toInt()
                    if (prefix !in 0..32) return@mapNotNull null
                    InterfaceAddress(face.name, address, IPv4Addr(IPv4Network.mask(prefix)))
                }
            }
    }.getOrDefault(emptyList())
}
