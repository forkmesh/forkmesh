package com.forkmesh.nativeapp.openwrt

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.util.concurrent.atomic.AtomicInteger
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.SSLSocket
import javax.net.ssl.X509TrustManager

class OpenWrtHttpResponse(val status: Int, val headers: Map<String, String>, val body: ByteArray) {
    fun header(name: String): String? = headers[name.lowercase()]
}

class OpenWrtCertificateException(val fingerprint: String) : IOException("The router's certificate is not trusted yet.")

fun interface OpenWrtTransport {
    fun post(url: String, contentType: String, body: ByteArray, timeoutMs: Int): OpenWrtHttpResponse
}

class RouterHttp(private val trustedFingerprint: String) : OpenWrtTransport {
    override fun post(url: String, contentType: String, body: ByteArray, timeoutMs: Int): OpenWrtHttpResponse {
        val uri = URI(url)
        val https = uri.scheme.equals("https", ignoreCase = true)
        val host = uri.host.trim('[', ']')
        val port = if (uri.port > 0) uri.port else if (https) 443 else 80
        val plain = Socket()
        try {
            plain.connect(InetSocketAddress(host, port), minOf(timeoutMs, 10_000))
            plain.soTimeout = timeoutMs
            val socket = if (https) secure(plain, host, port) else plain
            socket.use { connection ->
                val path = (uri.rawPath ?: "").ifEmpty { "/" }
                val authority = uri.rawAuthority ?: host
                val head = "POST $path HTTP/1.0\r\nHost: $authority\r\nContent-Type: $contentType\r\nContent-Length: ${body.size}\r\n" +
                    "Accept: application/json\r\nConnection: close\r\nUser-Agent: ForkMesh\r\n\r\n"
                val output = connection.getOutputStream()
                output.write(head.toByteArray(Charsets.ISO_8859_1))
                output.write(body)
                output.flush()
                return parse(readAll(connection.getInputStream(), MAX_RESPONSE))
            }
        } finally {
            if (!plain.isClosed) plain.close()
        }
    }

    private fun secure(socket: Socket, host: String, port: Int): SSLSocket {
        var seen = ""
        val trust = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                throw CertificateException("client certificates are not used")
            }

            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                val leaf = chain?.firstOrNull() ?: throw CertificateException("no certificate")
                seen = OpenWrtModel.certificateFingerprint(leaf.encoded)
                if (trustedFingerprint.isEmpty() || seen != trustedFingerprint) throw CertificateException("certificate not trusted")
            }

            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
        val context = SSLContext.getInstance("TLS")
        context.init(null, arrayOf(trust), SecureRandom())
        val ssl = context.socketFactory.createSocket(socket, host, port, true) as SSLSocket
        try {
            ssl.startHandshake()
        } catch (error: SSLException) {
            if (seen.isNotEmpty() && seen != trustedFingerprint) throw OpenWrtCertificateException(seen)
            throw error
        }
        return ssl
    }

    companion object {
        const val MAX_RESPONSE = 16 * 1024 * 1024

        fun readAll(input: InputStream, limit: Int): ByteArray {
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(16 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                out.write(buffer, 0, read)
                if (out.size() > limit) throw IOException("The router's answer is too large.")
            }
            return out.toByteArray()
        }

        fun parse(raw: ByteArray): OpenWrtHttpResponse {
            val split = indexOf(raw, byteArrayOf(13, 10, 13, 10))
            if (split < 0) throw IOException("The router's answer was not HTTP.")
            val lines = String(raw, 0, split, Charsets.ISO_8859_1).split("\r\n")
            val status = lines.first().split(' ').getOrNull(1)?.toIntOrNull() ?: throw IOException("The router's answer was not HTTP.")
            val headers = lines.drop(1).mapNotNull { line ->
                val colon = line.indexOf(':')
                if (colon <= 0) null else line.substring(0, colon).trim().lowercase() to line.substring(colon + 1).trim()
            }.toMap()
            var body = raw.copyOfRange(split + 4, raw.size)
            if (headers["transfer-encoding"]?.contains("chunked", ignoreCase = true) == true) body = dechunk(body)
            return OpenWrtHttpResponse(status, headers, body)
        }

        private fun dechunk(data: ByteArray): ByteArray {
            val out = ByteArrayOutputStream()
            var position = 0
            while (position < data.size) {
                val end = indexOf(data, byteArrayOf(13, 10), position)
                if (end < 0) break
                val size = String(data, position, end - position, Charsets.ISO_8859_1).substringBefore(';').trim().toIntOrNull(16) ?: break
                if (size == 0) break
                val start = end + 2
                if (start + size > data.size) break
                out.write(data, start, size)
                position = start + size + 2
            }
            return out.toByteArray()
        }

        private fun indexOf(data: ByteArray, pattern: ByteArray, from: Int = 0): Int {
            outer@ for (i in from..data.size - pattern.size) {
                for (j in pattern.indices) if (data[i + j] != pattern[j]) continue@outer
                return i
            }
            return -1
        }
    }
}

data class OpenWrtSnapshot(
    val board: JsonMap = emptyMap(),
    val info: JsonMap = emptyMap(),
    val interfaces: List<OpenWrtInterfaceStatus> = emptyList(),
    val wirelessStatus: JsonMap = emptyMap(),
    val configs: Map<String, JsonMap> = emptyMap(),
    val configNames: List<String> = emptyList(),
    val missingConfigs: Set<String> = emptySet(),
    val changes: JsonMap = emptyMap(),
    val leases: List<OpenWrtLease> = emptyList(),
    val services: List<OpenWrtService> = emptyList(),
    val packages: List<OpenWrtPackage> = emptyList(),
    val packageTool: OpenWrtPackageTool? = null,
    val blocks: List<OpenWrtBlockDevice> = emptyList(),
    val mounts: List<OpenWrtMountPoint> = emptyList(),
    val storageProblem: String = "",
    val problems: List<String> = emptyList(),
) {
    fun sections(config: String, type: String = ""): List<UciSection> = OpenWrtModel.uciSections(configs[config] ?: emptyMap(), type)

    fun section(config: String, name: String): UciSection? = if (name.isEmpty()) null else sections(config).firstOrNull { it.name == name }

    val hostname: String get() = sections("system", "system").firstOrNull()?.option("hostname").orEmpty().ifEmpty { board.str("hostname") }
    val pendingCount: Int get() = OpenWrtModel.pendingCount(changes)
    val pendingLines: List<String> get() = OpenWrtModel.describeChanges(changes)
    val interfaceSections: List<UciSection> get() = sections("network", "interface").filter { it.name != "loopback" }
    val radios: List<UciSection> get() = sections("wireless", "wifi-device")
    fun wifiNetworks(radio: String): List<UciSection> = sections("wireless", "wifi-iface").filter { it.option("device") == radio }
    val reservations: List<UciSection> get() = sections("dhcp", "host")
    val forwards: List<UciSection> get() = sections("firewall", "redirect").filter { it.option("target", "DNAT") == "DNAT" }
    val disks: List<OpenWrtDisk> get() = OpenWrtModel.storageDisks(blocks, mounts, sections("fstab"))
    val shareConfig: String get() = OpenWrtModel.shareConfig(configNames)
    val shares: List<OpenWrtShare> get() = if (shareConfig.isEmpty()) emptyList() else OpenWrtModel.parseShares(sections(shareConfig))
    val autoMount: Boolean get() = sections("fstab", "global").firstOrNull()?.flag("anon_mount") ?: false
    val fstabMissing: Boolean get() = "fstab" in missingConfigs

    fun dhcpSection(forInterface: String): UciSection? = sections("dhcp", "dhcp").firstOrNull { it.option("interface") == forInterface }
    fun status(of: String): OpenWrtInterfaceStatus? = interfaces.firstOrNull { it.name == of }
    fun reservation(mac: String): UciSection? = reservations.firstOrNull { mac.lowercase() in it.option("mac").lowercase().split(' ') }
}

sealed class OpenWrtApplyOutcome(val message: String, val isError: Boolean) {
    data object NothingToApply : OpenWrtApplyOutcome("There was nothing to apply.", false)
    data object Applied : OpenWrtApplyOutcome("Applied. The router is reloading the changed services.", false)
    data object Confirmed : OpenWrtApplyOutcome("Applied and confirmed. The router kept the new settings.", false)
    class RolledBack(message: String) : OpenWrtApplyOutcome(message, true)
    class Failed(message: String) : OpenWrtApplyOutcome(message, true)
}

class OpenWrtClient(private val transport: OpenWrtTransport, base: String) {
    @Volatile
    var base: String = base
        private set

    @Volatile
    var user: String = ""
        private set

    @Volatile
    private var session: String = ""

    @Volatile
    private var password: String = ""

    @Volatile
    var renewals: Int = 0
        private set

    private val ids = AtomicInteger(1)
    private val renewLock = Any()

    val signedIn: Boolean get() = session.isNotEmpty()

    fun rememberPassword(value: String) {
        password = value
    }

    fun signIn(user: String, password: String): OpenWrtResult {
        session = ""
        this.password = ""
        this.user = user
        val result = login(user, password)
        if (result.ok) {
            val fresh = result.data.str("ubus_rpc_session")
            if (fresh.isEmpty()) return result.copy(ok = false, error = "The router accepted the sign-in but returned no session.")
            session = fresh
            this.password = password
            return result
        }
        return if (result.accessDenied) result.copy(error = "The user name or password is wrong.") else result
    }

    fun signOut() {
        if (session.isNotEmpty()) send("session", "destroy", emptyMap(), 5_000, session)
        session = ""
        password = ""
    }

    fun call(objectName: String, method: String, args: Map<String, Any?> = emptyMap(), timeoutMs: Int = 20_000): OpenWrtResult {
        val used = session
        val result = send(objectName, method, args, timeoutMs, used)
        if (!result.accessDenied || password.isEmpty()) return result
        val renewed = renew(used)
        if (!renewed.ok) return renewed.copy(ok = false, accessDenied = true, error = "The router session ended and signing in again failed: ${renewed.error}")
        return send(objectName, method, args, timeoutMs, session)
    }

    fun exec(command: String, params: List<String>, timeoutMs: Int = 120_000): OpenWrtResult {
        val result = call("file", "exec", mapOf("command" to command, "params" to params), timeoutMs)
        if (!result.ok) return result
        val code = result.data.int("code")
        if (code == 0) return result
        val errors = result.data.str("stderr").trim()
        return result.copy(ok = false, error = if (errors.isEmpty()) "The command exited with code $code." else errors.takeLast(600))
    }

    private fun renew(stale: String): OpenWrtResult = synchronized(renewLock) {
        if (session != stale && session.isNotEmpty()) return OpenWrtResult.OK
        val result = login(user, password)
        val fresh = result.data.str("ubus_rpc_session")
        if (result.ok && fresh.isNotEmpty()) {
            session = fresh
            renewals += 1
            result
        } else {
            result.copy(ok = false)
        }
    }

    private fun login(user: String, password: String): OpenWrtResult =
        send("session", "login", mapOf("username" to user, "password" to password, "timeout" to OpenWrt.SESSION_TIMEOUT.toLong()), 20_000, OpenWrt.ANONYMOUS_SESSION)

    private fun send(objectName: String, method: String, args: Map<String, Any?>, timeoutMs: Int, session: String): OpenWrtResult {
        val body = OpenWrtModel.callBody(ids.getAndIncrement(), session, objectName, method, args)
        val host = try {
            URI(base).host ?: base
        } catch (_: Exception) {
            base
        }
        return try {
            val response = transport.post("$base/ubus", "application/json", body, timeoutMs)
            if (response.status in 301..308) {
                val location = response.header("location").orEmpty()
                OpenWrtResult.failure(
                    if (location.startsWith("https:", ignoreCase = true)) "The router only answers over HTTPS. Change the address to https://${host.trim('[', ']')}."
                    else "The router redirected the request to $location. Use that address instead.",
                )
            } else {
                OpenWrtModel.parseReply(response.body, response.status)
            }
        } catch (error: OpenWrtCertificateException) {
            OpenWrtResult(
                error = "The router's HTTPS certificate is not trusted yet. Compare its fingerprint with the one the router shows, then choose Trust.",
                certificate = error.fingerprint,
            )
        } catch (error: IOException) {
            OpenWrtResult(error = "Could not reach the router at ${host.trim('[', ']')}: ${error.message ?: error.javaClass.simpleName}", unreachable = true)
        }
    }

    fun loadSnapshot(now: Long = System.currentTimeMillis() / 1000, preferredTool: OpenWrtPackageTool? = null): OpenWrtSnapshot {
        val problems = mutableListOf<String>()
        val board = call("system", "board")
        if (!board.ok) problems += board.error
        val configNames = call("uci", "configs").data.strings("configs").sorted()
        val wanted = BASE_CONFIGS + listOfNotNull(OpenWrtModel.shareConfig(configNames).ifEmpty { null })
        val configs = linkedMapOf<String, JsonMap>()
        val missingConfigs = mutableSetOf<String>()
        for (config in wanted) {
            val result = call("uci", "get", mapOf("config" to config))
            if (result.ok || result.status == OpenWrt.STATUS_NOT_FOUND) {
                configs[config] = result.data.obj("values")
                if (!result.ok) missingConfigs += config
            } else {
                problems += "Could not read the $config settings: ${result.error}"
            }
        }
        val luciLeases = call("luci-rpc", "getDHCPLeases")
        val leases = if (luciLeases.ok) {
            OpenWrtModel.parseLuciLeases(luciLeases.data, now)
        } else {
            val file = call("file", "read", mapOf("path" to "/tmp/dhcp.leases"))
            if (file.ok) OpenWrtModel.parseLeases(file.data.str("data")) else emptyList()
        }
        val blocks = call("luci", "getBlockDevices")
        val packages = listPackages(preferredTool)
        return OpenWrtSnapshot(
            board = board.data,
            info = call("system", "info").data,
            interfaces = OpenWrtModel.parseInterfaces(call("network.interface", "dump").data),
            wirelessStatus = call("network.wireless", "status").data,
            configs = configs,
            configNames = configNames,
            missingConfigs = missingConfigs,
            changes = call("uci", "changes").data.obj("changes"),
            leases = leases,
            services = OpenWrtModel.parseServices(call("rc", "list").data),
            packages = packages.second,
            packageTool = packages.first,
            blocks = if (blocks.ok) OpenWrtModel.parseBlockDevices(blocks.data) else emptyList(),
            mounts = OpenWrtModel.parseMountPoints(call("luci", "getMountPoints").data),
            storageProblem = if (blocks.ok) "" else blocks.error,
            problems = problems,
        )
    }

    fun listPackages(preferred: OpenWrtPackageTool?): Pair<OpenWrtPackageTool?, List<OpenWrtPackage>> {
        for (tool in preferred?.let { listOf(it) } ?: OpenWrtModel.packageTools) {
            val result = exec(tool.program, OpenWrtModel.packageArguments(tool, "list"))
            val output = result.data.str("stdout")
            if (result.ok && output.isNotBlank()) return tool to OpenWrtModel.parseInstalledPackages(output)
        }
        return null to emptyList()
    }

    fun configValues(config: String): JsonMap = call("uci", "get", mapOf("config" to config)).data.obj("values")

    fun changes(): JsonMap = call("uci", "changes").data.obj("changes")

    fun stage(config: String, section: String, edit: UciEdit): OpenWrtResult {
        var result = OpenWrtResult.OK
        if (edit.values.isNotEmpty()) {
            result = call("uci", "set", mapOf("config" to config, "section" to section, "values" to edit.values))
            if (!result.ok) return result
        }
        if (edit.removed.isNotEmpty()) result = call("uci", "delete", mapOf("config" to config, "section" to section, "options" to edit.removed))
        return result
    }

    fun addSection(config: String, type: String, name: String = "", values: Map<String, Any?>): OpenWrtResult {
        val args = linkedMapOf<String, Any?>("config" to config, "type" to type, "values" to values)
        if (name.isNotEmpty()) args["name"] = name
        return call("uci", "add", args)
    }

    fun deleteSection(config: String, section: String): OpenWrtResult = call("uci", "delete", mapOf("config" to config, "section" to section))

    fun discard(changes: JsonMap): OpenWrtResult {
        var last = OpenWrtResult.OK
        for (config in changes.keys) {
            val result = call("uci", "revert", mapOf("config" to config))
            if (!result.ok) last = result
        }
        return last
    }

    fun apply(rollback: Boolean, wait: (Double) -> Unit): OpenWrtApplyOutcome {
        val result = call("uci", "apply", mapOf("rollback" to rollback, "timeout" to OpenWrt.ROLLBACK_SECONDS.toLong()))
        if (!result.ok) return if (result.status == OpenWrt.STATUS_NO_DATA) OpenWrtApplyOutcome.NothingToApply else OpenWrtApplyOutcome.Failed(result.error)
        if (!rollback) return OpenWrtApplyOutcome.Applied
        val budget = (OpenWrt.ROLLBACK_SECONDS - 10).toDouble()
        var waited = 3.0
        wait(3.0)
        while (true) {
            val confirm = call("uci", "confirm", emptyMap(), 4_000)
            if (confirm.ok) return OpenWrtApplyOutcome.Confirmed
            if ((confirm.unreachable || confirm.accessDenied) && waited < budget) {
                wait(2.0)
                waited += 2.0
                continue
            }
            return if (confirm.unreachable) {
                OpenWrtApplyOutcome.RolledBack("The router stopped answering after the change, so it will put the old settings back by itself within ${OpenWrt.ROLLBACK_SECONDS} seconds.")
            } else {
                OpenWrtApplyOutcome.Failed("The change could not be confirmed (${confirm.error}). The router will put the old settings back by itself.")
            }
        }
    }

    fun createFstab(): OpenWrtResult {
        val detect = exec("/sbin/block", listOf("detect"), 60_000)
        if (!detect.ok) {
            return OpenWrtResult.failure("Automatic mounting needs block-mount, and the router could not run it (${detect.error}). Press Install USB storage support first.")
        }
        val written = call("file", "write", mapOf("path" to OpenWrtModel.FSTAB_PATH, "data" to OpenWrtModel.fstabFromDetect(detect.data.str("stdout")), "mode" to 420))
        if (!written.ok) {
            return OpenWrtResult.failure("The router has no /etc/config/fstab and did not let ForkMesh create it (${written.error}). Reinstall block-mount, which creates it, or run \"block detect > /etc/config/fstab\" on the router.")
        }
        return written
    }

    fun checkStorage(current: OpenWrtSnapshot): Pair<OpenWrtStorageCheck, OpenWrtSnapshot> {
        var snapshot = current
        val tool = snapshot.packageTool
        if (tool != null) {
            val listed = listPackages(tool)
            if (listed.first != null) snapshot = snapshot.copy(packages = listed.second)
        }
        val fstab = call("uci", "get", mapOf("config" to "fstab"))
        if (fstab.ok || fstab.status == OpenWrt.STATUS_NOT_FOUND) {
            snapshot = snapshot.copy(
                configs = snapshot.configs + ("fstab" to fstab.data.obj("values")),
                missingConfigs = if (fstab.ok) snapshot.missingConfigs - "fstab" else snapshot.missingConfigs + "fstab",
            )
        }
        val blockTool = call("file", "stat", mapOf("path" to "/sbin/block")).ok
        val usb = call("file", "list", mapOf("path" to "/sys/bus/usb/devices"))
        val entries = usb.data.maps("entries").map { it.str("name") }
        val devices = OpenWrtModel.usbDeviceEntries(entries).take(8).map { entry ->
            val product = call("file", "read", mapOf("path" to "/sys/bus/usb/devices/$entry/product"))
            val name = if (product.ok) product.data.str("data").trim() else ""
            if (name.isEmpty()) entry else "$name ($entry)"
        }
        val partitions = call("file", "read", mapOf("path" to "/proc/partitions"))
        val blocks = call("luci", "getBlockDevices")
        snapshot = snapshot.copy(
            blocks = if (blocks.ok) OpenWrtModel.parseBlockDevices(blocks.data) else emptyList(),
            storageProblem = if (blocks.ok) "" else blocks.error,
            mounts = OpenWrtModel.parseMountPoints(call("luci", "getMountPoints").data),
            changes = call("uci", "changes").data.obj("changes"),
        )
        val disks = snapshot.disks
        val check = OpenWrtStorageCheck(
            packagesKnown = tool != null,
            installed = snapshot.packages.map { it.name },
            blockTool = blockTool,
            fstabExists = fstab.ok,
            autoMount = snapshot.autoMount,
            autoMountPending = snapshot.changes.containsKey("fstab"),
            usbKnown = usb.ok,
            usbControllers = OpenWrtModel.usbControllerCount(entries),
            usbDevices = devices,
            partitionsKnown = partitions.ok,
            partitions = OpenWrtModel.parsePartitions(partitions.data.str("data")),
            disks = disks.size,
            mounted = disks.count { it.mounted },
            listProblem = snapshot.storageProblem,
            kernelLines = OpenWrtModel.storageKernelLines(exec("/bin/dmesg", listOf("-r")).data.str("stdout")),
        )
        return check to snapshot
    }

    fun packageAction(tool: OpenWrtPackageTool, action: String, names: List<String>): OpenWrtResult {
        if (action != "update" && (names.isEmpty() || !names.all(OpenWrtModel::validPackageName))) {
            return OpenWrtResult.failure(if (names.isEmpty()) "Choose a package first." else "That is not a package name.")
        }
        val result = exec(tool.program, OpenWrtModel.packageArguments(tool, action, names), 300_000)
        if (action != "install" || result.ok || !result.timedOut) return result
        val deadline = System.currentTimeMillis() + 180_000
        while (true) {
            if (OpenWrtModel.packagesInstalled(listPackages(tool).second, names)) return OpenWrtResult.OK
            if (System.currentTimeMillis() >= deadline) break
            Thread.sleep(5_000)
        }
        return result.copy(
            error = "The router stopped answering during the install, and ${names.joinToString(", ")} still is not installed after three minutes. " +
                "The router may have no internet connection, or the download is still running: check Packages later, or try again.",
        )
    }

    companion object {
        val BASE_CONFIGS = listOf("network", "wireless", "dhcp", "firewall", "system", "fstab")

        fun resolve(host: String): List<String> = try {
            InetAddress.getAllByName(host).mapNotNull { it.hostAddress?.substringBefore('%') }.distinct()
        } catch (_: Exception) {
            emptyList()
        }
    }
}
