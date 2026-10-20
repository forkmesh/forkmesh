package com.forkmesh.nativeapp.nexus

sealed class TargetHost : Comparable<TargetHost> {
    data class Address(val address: IPAddr) : TargetHost() {
        override fun toString(): String = address.toString()
    }

    data class Name(val name: String) : TargetHost() {
        override fun toString(): String = name
    }

    override fun compareTo(other: TargetHost): Int = when {
        this is Address && other is Address -> address.compareTo(other.address)
        this is Name && other is Name -> name.compareTo(other.name)
        this is Address -> -1
        else -> 1
    }
}

class TargetParseException(message: String) : IllegalArgumentException(message)

class TargetSpecification(val components: List<Component>, raw: String? = null) {
    sealed class Component {
        data class Address(val address: IPAddr) : Component()
        data class Name(val name: String) : Component()
        data class Network(val network: IPv4Network) : Component()
        data class Network6(val network: IPv6Network) : Component()
        data class Range(val start: IPv4Addr, val end: IPv4Addr) : Component()

        val hostCount: Long
            get() = when (this) {
                is Address, is Name -> 1
                is Network -> network.hostCount
                is Network6 -> network.addressCount ?: Long.MAX_VALUE
                is Range -> end.value - start.value + 1
            }

        val isRange: Boolean get() = this is Network || this is Network6 || this is Range

        fun render(): String = when (this) {
            is Address -> address.toString()
            is Name -> name
            is Network -> network.toString()
            is Network6 -> network.toString()
            is Range -> "$start-$end"
        }
    }

    val raw: String = raw ?: components.joinToString(", ") { it.render() }

    val hostCount: Long
        get() = components.fold(0L) { total, component ->
            val sum = total + component.hostCount
            if (sum < total || component.hostCount == Long.MAX_VALUE) Long.MAX_VALUE else sum
        }

    val containsRange: Boolean get() = components.any { it.isRange }

    fun hosts(): List<TargetHost> {
        val seen = LinkedHashSet<TargetHost>()
        for (component in components) {
            when (component) {
                is Component.Address -> seen += TargetHost.Address(component.address)
                is Component.Name -> seen += TargetHost.Name(component.name)
                is Component.Network -> component.network.hosts().forEach { seen += TargetHost.Address(IPAddr.V4(it)) }
                is Component.Network6 -> component.network.addresses(256).forEach { seen += TargetHost.Address(IPAddr.V6(it)) }
                is Component.Range -> (component.start.value..component.end.value).forEach { seen += TargetHost.Address(IPAddr.V4(IPv4Addr(it))) }
            }
        }
        return seen.toList()
    }

    override fun toString(): String = raw

    companion object {
        fun parse(text: String, maximumHosts: Int = 1024): TargetSpecification {
            val tokens = text.split(',', ' ', '\t', '\n').filter { it.isNotEmpty() }
            if (tokens.isEmpty()) throw TargetParseException("Enter an IP address, hostname, range or CIDR network.")
            val components = mutableListOf<Component>()
            for (token in tokens) {
                val component = parseToken(token)
                if (component !in components) components += component
            }
            val spec = TargetSpecification(components, tokens.joinToString(", "))
            val count = spec.hostCount
            if (count > maximumHosts) throw TargetParseException("The targets cover $count hosts; the limit is $maximumHosts.")
            return spec
        }

        fun parseToken(token: String): Component {
            IPAddr.parse(token)?.let { return Component.Address(it) }
            if ('/' in token) {
                IPv4Network.parse(token)?.let { return if (it.prefixLength == 32) Component.Address(IPAddr.V4(it.network)) else Component.Network(it) }
                IPv6Network.parse(token)?.let {
                    if (it.prefixLength < 120) throw TargetParseException("“$token” is too large; IPv6 networks must be /120 or smaller.")
                    return if (it.prefixLength == 128) Component.Address(IPAddr.V6(it.network)) else Component.Network6(it)
                }
                throw TargetParseException(invalid(token))
            }
            val dash = token.indexOf('-')
            val start = if (dash > 0) IPv4Addr.parse(token.substring(0, dash)) else null
            if (start != null) {
                val tail = token.substring(dash + 1)
                val end = IPv4Addr.parse(tail) ?: if (tail.isNotEmpty() && tail.length <= 3 && tail.all { it in '0'..'9' } && tail.toInt() <= 255) {
                    IPv4Addr((start.value and 0xFFFF_FF00L) or tail.toLong())
                } else {
                    throw TargetParseException(invalid(token))
                }
                if (end < start) throw TargetParseException("The range “$token” ends before it starts.")
                return if (start == end) Component.Address(IPAddr.V4(start)) else Component.Range(start, end)
            }
            Hostname.normalized(token)?.let { return Component.Name(it) }
            throw TargetParseException(invalid(token))
        }

        private fun invalid(token: String) = "“$token” is not an IP address, hostname, range or CIDR network."
    }
}

enum class TargetScope(val rank: Int) {
    Loopback(0), LocalNetwork(1), AuthorizedNetwork(2), PublicInternet(3);

    companion object {
        fun classify(address: IPAddr, authorizedNetworks: List<IPNetwork> = emptyList()): TargetScope = when {
            address.isLoopback -> Loopback
            address.isLocalScope -> LocalNetwork
            authorizedNetworks.any { it.contains(address) } -> AuthorizedNetwork
            else -> PublicInternet
        }
    }
}

sealed class AuthorizationDecision {
    object Allowed : AuthorizationDecision()
    data class RequiresAttestation(val message: String) : AuthorizationDecision()
    data class Denied(val message: String) : AuthorizationDecision()

    val isAllowed: Boolean get() = this == Allowed
}

data class ScanAuthorizationPolicy(
    val allowPublicTargets: Boolean = false,
    val authorizedNetworks: List<IPNetwork> = emptyList(),
    val maximumHostsPerScan: Int = 256,
    val maximumPublicHostsPerScan: Int = 4,
    val maximumProbesPerScan: Int = 70_000,
) {
    fun evaluate(addresses: List<IPAddr>, portCount: Int, isRange: Boolean, attestation: AuthorizationAttestation?): AuthorizationDecision {
        if (addresses.isEmpty()) return AuthorizationDecision.Denied("No addresses to scan.")
        addresses.firstOrNull { it.isMulticastOrBroadcast || it.isUnspecified }?.let {
            return AuthorizationDecision.Denied("$it is a multicast, broadcast or unspecified address and cannot be scanned.")
        }
        if (addresses.size > maximumHostsPerScan) {
            return AuthorizationDecision.Denied("${addresses.size} hosts exceeds the limit of $maximumHostsPerScan per scan.")
        }
        val probes = addresses.size.toLong() * portCount
        if (probes > maximumProbesPerScan) {
            return AuthorizationDecision.Denied("$probes connection attempts exceeds the limit of $maximumProbesPerScan per scan.")
        }
        val publicCount = addresses.count { TargetScope.classify(it, authorizedNetworks) == TargetScope.PublicInternet }
        if (publicCount == 0) return AuthorizationDecision.Allowed
        if (!allowPublicTargets) {
            return AuthorizationDecision.Denied("Internet addresses are off. Turn on “Allow internet targets” in Settings, or add the network to Authorized networks.")
        }
        if (isRange) {
            return AuthorizationDecision.Denied("Ranges of internet addresses can only be scanned when the network is listed under Authorized networks.")
        }
        if (publicCount > maximumPublicHostsPerScan) {
            return AuthorizationDecision.Denied("$publicCount internet hosts exceeds the limit of $maximumPublicHostsPerScan per scan.")
        }
        if (attestation == null) {
            return AuthorizationDecision.RequiresAttestation("Confirm you own or are authorized to test the internet hosts in this scan.")
        }
        return AuthorizationDecision.Allowed
    }
}
