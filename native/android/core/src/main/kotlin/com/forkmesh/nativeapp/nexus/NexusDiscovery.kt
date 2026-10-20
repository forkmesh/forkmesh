package com.forkmesh.nativeapp.nexus

data class DiscoveryOptions(
    val serviceTypes: List<String> = DEFAULT_TYPES,
    val durationMs: Long = 8_000,
) {
    companion object {
        val DEFAULT_TYPES = listOf("_http._tcp", "_https._tcp", "_ssh._tcp", "_smb._tcp", "_airplay._tcp", "_raop._tcp", "_ipp._tcp", "_homekit._tcp")
    }
}

data class DiscoveryProgress(val serviceType: String, val devices: List<Device>, val completedServiceTypes: Int, val totalServiceTypes: Int) {
    val fraction: Double get() = if (totalServiceTypes == 0) 1.0 else completedServiceTypes.toDouble() / totalServiceTypes
}

fun interface ServiceBrowsing {
    fun browse(type: String, durationMs: Long, cancel: NexusCancel): List<NetService>
}

class DiscoveryEngine(private val browser: ServiceBrowsing) {
    fun discover(options: DiscoveryOptions = DiscoveryOptions(), cancel: NexusCancel = NexusCancel(), progress: (DiscoveryProgress) -> Unit = {}): DiscoverySession {
        val inventory = DeviceInventory()
        var session = DiscoverySession(methods = listOf(DiscoverySource.Mdns), serviceTypes = options.serviceTypes, network = LocalNetwork.current()?.toString())
        try {
            options.serviceTypes.forEachIndexed { index, type ->
                cancel.check()
                browser.browse(type, options.durationMs, cancel).forEach(inventory::merge)
                progress(DiscoveryProgress(type, inventory.devices, index + 1, options.serviceTypes.size))
            }
            session = session.copy(status = SessionStatus.Completed)
        } catch (_: ConnectionFailure.Cancelled) {
            session = session.copy(status = SessionStatus.Cancelled)
        } catch (error: Exception) {
            session = session.copy(status = SessionStatus.Failed(NexusFailures.map(error).detail))
        }
        if (cancel.isCancelled) session = session.copy(status = SessionStatus.Cancelled)
        return session.copy(devices = inventory.devices, finishedAt = System.currentTimeMillis())
    }
}

class DeviceInventory(devices: List<Device> = emptyList()) {
    var devices: List<Device> = devices
        private set

    fun merge(service: NetService) {
        val index = devices.indexOfFirst { device ->
            device.addresses.any { it in service.addresses } ||
                (service.hostName != null && service.hostName in device.hostnames) ||
                device.services.any { it.id == service.id }
        }
        devices = if (index >= 0) {
            devices.toMutableList().also { list ->
                val device = list[index]
                val services = device.services.filter { it.id != service.id } + service
                list[index] = device.copy(
                    services = services,
                    addresses = (device.addresses + service.addresses).distinct().sorted(),
                    hostnames = if (service.hostName != null && service.hostName !in device.hostnames) device.hostnames + service.hostName else device.hostnames,
                    sources = if (DiscoverySource.Mdns in device.sources) device.sources else device.sources + DiscoverySource.Mdns,
                    identification = DeviceClassifier.identify(services),
                    lastSeen = System.currentTimeMillis(),
                )
            }
        } else {
            val id = service.addresses.firstOrNull()?.let(Device::key) ?: "mdns:${service.id}"
            devices + Device(
                id = id, displayName = service.name, addresses = service.addresses, hostnames = listOfNotNull(service.hostName),
                services = listOf(service), sources = listOf(DiscoverySource.Mdns), identification = DeviceClassifier.identify(listOf(service)),
            )
        }
    }

    fun changes(previous: List<Device>): List<ObservedChange> {
        val old = previous.associateBy { it.id }
        val fresh = devices.associateBy { it.id }
        val changes = mutableListOf<ObservedChange>()
        devices.filter { it.id !in old }.forEach { changes += ObservedChange(ChangeKind.DeviceAppeared, it.id, it.title) }
        previous.filter { it.id !in fresh }.forEach { changes += ObservedChange(ChangeKind.DeviceDisappeared, it.id, it.title) }
        for (device in devices) {
            val prior = old[device.id] ?: continue
            val before = prior.services.map { it.id }.toSet()
            val after = device.services.map { it.id }.toSet()
            (after - before).forEach { changes += ObservedChange(ChangeKind.ServiceAdded, device.id, device.title, after = it) }
            (before - after).forEach { changes += ObservedChange(ChangeKind.ServiceRemoved, device.id, device.title, before = it) }
        }
        return changes.sortedBy { it.subject }
    }
}

object DeviceClassifier {
    private val rules: List<Pair<DeviceKind, List<String>>> = listOf(
        DeviceKind.Printer to listOf("_ipp", "_ipps", "_printer", "_pdl-datastream", "_scanner", "_uscan"),
        DeviceKind.Tv to listOf("_googlecast", "_airplay", "_androidtvremote", "_androidtvremote2", "_roku"),
        DeviceKind.Speaker to listOf("_raop", "_spotify-connect", "_sonos"),
        DeviceKind.SmartHome to listOf("_homekit", "_hap", "_matter", "_matterc", "_hue", "_esphomelib"),
        DeviceKind.Camera to listOf("_rtsp", "_axis-video"),
        DeviceKind.Storage to listOf("_afpovertcp", "_nfs", "_adisk"),
        DeviceKind.Computer to listOf("_ssh", "_sftp-ssh", "_rfb", "_smb", "_workstation", "_device-info"),
    )

    fun identify(services: List<NetService>): DeviceIdentification? {
        val types = services.map { it.type.trim('.').substringBefore('.').lowercase() }.toSet()
        for ((kind, prefixes) in rules) {
            val match = prefixes.firstOrNull { it in types } ?: continue
            return DeviceIdentification(kind, EvidenceLevel.Inferred, "Advertises the $match service, typical of a ${kind.title.lowercase()}.")
        }
        return null
    }
}
