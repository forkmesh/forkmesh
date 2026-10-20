package com.forkmesh.nativeapp.openwrt

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException
import java.net.ServerSocket
import kotlin.concurrent.thread

private class FakeRouter : OpenWrtTransport {
    var session = ""
    var logins = 0
    var expireNextBoard = false
    var confirmFailures = 0
    var fstabMissing = false
    var ksmbdMissing = false
    var shareAclFromLogin = 0
    var configListHidesKsmbd = false
    val installed = mutableListOf<String>()
    val calls = mutableListOf<List<Any?>>()

    fun last(objectName: String, method: String): List<Any?>? = calls.lastOrNull { it.getOrNull(1) == objectName && it.getOrNull(2) == method }

    @Suppress("UNCHECKED_CAST")
    override fun post(url: String, contentType: String, body: ByteArray, timeoutMs: Int): OpenWrtHttpResponse {
        val request = Json.parseObject(String(body, Charsets.UTF_8))
        val params = request.list("params")
        calls += params
        val sid = params.getOrNull(0) as? String ?: ""
        val objectName = params.getOrNull(1) as? String ?: ""
        val method = params.getOrNull(2) as? String ?: ""
        val args = params.getOrNull(3) as? Map<String, Any?> ?: emptyMap()
        fun reply(result: Any?) = respond(mapOf("jsonrpc" to "2.0", "id" to request["id"], "result" to result))
        val denied = respond(mapOf("jsonrpc" to "2.0", "id" to request["id"], "error" to mapOf("code" to -32002L, "message" to "Access denied")))
        if (objectName == "session" && method == "login") {
            if (sid != OpenWrt.ANONYMOUS_SESSION || args.str("password") != "secret") return reply(listOf(6L))
            logins += 1
            session = "sess$logins"
            return reply(listOf(0L, mapOf("ubus_rpc_session" to session)))
        }
        if (sid != session) return denied
        if (objectName == "system" && method == "board") {
            if (expireNextBoard) {
                expireNextBoard = false
                session = "expired"
                return denied
            }
            return reply(listOf(0L, mapOf("hostname" to "OpenWrt", "model" to "Test Router")))
        }
        if (objectName == "uci" && method == "confirm") {
            if (confirmFailures > 0) {
                confirmFailures -= 1
                throw IOException("connection refused")
            }
            return reply(listOf(0L))
        }
        if (objectName == "uci" && method == "configs") {
            return reply(listOf(0L, mapOf("configs" to listOf("network", "fstab") + if (logins >= shareAclFromLogin && !ksmbdMissing && !configListHidesKsmbd) listOf("ksmbd") else emptyList())))
        }
        if (objectName == "uci" && method == "get") {
            val values: Map<String, Any?> = when (args.str("config")) {
                "network" -> mapOf(
                    "lan" to mapOf(".name" to "lan", ".type" to "interface", ".index" to 1L, "proto" to "static", "ipaddr" to "192.168.1.1", "netmask" to "255.255.255.0"),
                    "wan" to mapOf(".name" to "wan", ".type" to "interface", ".index" to 2L, "proto" to "dhcp"),
                )
                "fstab" -> if (fstabMissing) return reply(listOf(4L)) else mapOf("cfg01" to mapOf(".name" to "cfg01", ".type" to "global", ".anonymous" to true, ".index" to 0L, "anon_mount" to "1"))
                "ksmbd" -> if (logins < shareAclFromLogin) return reply(listOf(6L)) else if (ksmbdMissing) return reply(listOf(4L)) else mapOf("cfg02" to mapOf(".name" to "cfg02", ".type" to "share", ".index" to 0L, "name" to "Media", "path" to "/mnt/sda1", "guest_ok" to "yes"))
                else -> return reply(listOf(4L))
            }
            return reply(listOf(0L, mapOf("values" to values)))
        }
        if (objectName == "luci" && method == "getBlockDevices") {
            return reply(listOf(0L, mapOf(
                "sda1" to mapOf("dev" to "/dev/sda1", "uuid" to "1234", "label" to "Media", "type" to "exfat", "size" to 64_000_000_000L, "mount" to "/mnt/sda1"),
                "mtdblock3" to mapOf("dev" to "/dev/mtdblock3", "type" to "squashfs"),
            )))
        }
        if (objectName == "luci" && method == "getMountPoints") {
            return reply(listOf(0L, mapOf("result" to listOf(mapOf("device" to "/dev/sda1", "mount" to "/mnt/sda1", "size" to 64_000_000_000L, "avail" to 1_000_000_000L)))))
        }
        if (objectName == "file" && method == "exec") {
            return when (args.str("command")) {
                "/bin/false" -> reply(listOf(0L, mapOf("code" to 1L, "stderr" to "nope")))
                "/bin/opkg" -> {
                    val words = (args["params"] as? List<Any?>).orEmpty().map { it.toString() }
                    if (words.firstOrNull() == "install") {
                        installed += words.drop(1)
                        if (words.getOrNull(1).orEmpty().startsWith("luci-app-")) {
                            return respond(mapOf("jsonrpc" to "2.0", "id" to request["id"], "error" to mapOf("code" to -32003L, "message" to "ubus request timed out")))
                        }
                        return reply(listOf(0L, mapOf("code" to 0L)))
                    }
                    reply(listOf(0L, mapOf("code" to 0L, "stdout" to "base-files - 1-r1\nluci - git-24\n" + installed.joinToString("") { "$it - 1.0\n" })))
                }
                "/sbin/block" -> reply(listOf(0L, mapOf("code" to 0L, "stdout" to "config 'global'\n\toption\tanon_mount\t'0'\n")))
                "/bin/dmesg" -> reply(listOf(0L, mapOf("code" to 0L, "stdout" to "<6>[    5.1] usb 1-1: new high-speed USB device number 2\n<5>[    6.0] random: crng init done\n")))
                else -> reply(listOf(6L))
            }
        }
        if (objectName == "file" && method == "write" && args.str("path") == "/etc/config/ksmbd") {
            if (!args.str("data").startsWith("config globals\n")) return reply(listOf(6L))
            ksmbdMissing = false
            return reply(listOf(0L))
        }
        if (objectName == "file" && method == "write") {
            if (args.str("path") != "/etc/config/fstab" || !args.str("data").contains("global")) return reply(listOf(6L))
            fstabMissing = false
            return reply(listOf(0L))
        }
        if (objectName == "file" && method == "list" && args.str("path") == "/sys/bus/usb/devices") {
            return reply(listOf(0L, mapOf("entries" to listOf(mapOf("name" to "usb1"), mapOf("name" to "1-1"), mapOf("name" to "1-1:1.0")))))
        }
        if (objectName == "file" && method == "read" && args.str("path") == "/sys/bus/usb/devices/1-1/product") return reply(listOf(0L, mapOf("data" to "Flash Disk\n")))
        if (objectName == "file" && method == "read" && args.str("path") == "/proc/partitions") {
            return reply(listOf(0L, mapOf("data" to "major minor  #blocks  name\n\n  31        0       1024 mtdblock0\n   8        0   64000000 sda\n   8        1   63999000 sda1\n")))
        }
        if (objectName == "uci" && method == "changes") return reply(listOf(0L, mapOf("changes" to emptyMap<String, Any?>())))
        return reply(listOf(0L))
    }

    private fun respond(value: Any?) = OpenWrtHttpResponse(200, mapOf("content-type" to "application/json"), Json.stringify(value).toByteArray())
}

class OpenWrtTest {
    fun testCallBodyAndReplies() {
        val body = Json.parseObject(String(OpenWrtModel.callBody(7, "", "uci", "get", mapOf("config" to "network"))))
        assertEq("call", body.str("method"))
        assertEq(OpenWrt.ANONYMOUS_SESSION, body.list("params")[0])
        val ok = OpenWrtModel.parseReply("""{"jsonrpc":"2.0","id":1,"result":[0,{"a":1}]}""".toByteArray())
        expect(ok.ok && ok.data["a"] == 1L)
        expect(OpenWrtModel.parseReply("""{"result":[6]}""".toByteArray()).accessDenied)
        expect(OpenWrtModel.parseReply("""{"error":{"code":-32002,"message":"Access denied"}}""".toByteArray()).accessDenied)
        expect(OpenWrtModel.parseReply("""{"error":{"code":-32000}}""".toByteArray()).error.contains("not found"))
        val slow = OpenWrtModel.parseReply("""{"error":{"code":-32003,"message":"ubus request timed out"}}""".toByteArray())
        expect(slow.timedOut && !slow.ok && !slow.accessDenied)
        expect(OpenWrtModel.parseReply("""{"result":[7]}""".toByteArray()).timedOut)
        val listed = listOf(OpenWrtPackage("ksmbd-server", "1"), OpenWrtPackage("luci-app-ksmbd", "1"))
        expect(OpenWrtModel.packagesInstalled(listed, listOf("luci-app-ksmbd")) && !OpenWrtModel.packagesInstalled(listed, listOf("luci-app-ksmbd", "nano")))
        expect(OpenWrtModel.parseReply("<html>".toByteArray(), 404).error.contains("/ubus"))
    }

    fun testAddresses() {
        val plain = OpenWrtModel.parseAddress("192.168.1.1")
        expect(plain.valid && !plain.encrypted)
        assertEq("http://192.168.1.1", plain.base)
        val tls = OpenWrtModel.parseAddress("https://router.lan:8443/cgi-bin/luci")
        expect(tls.valid && tls.encrypted)
        assertEq("https://router.lan:8443", tls.base)
        assertEq("https://192.168.1.1:8443", OpenWrtModel.baseWithHost(tls, "192.168.1.1"))
        expect(!OpenWrtModel.parseAddress("8.8.8.8").valid)
        expect(!OpenWrtModel.parseAddress("http://root:pw@192.168.1.1").valid)
        expect(!OpenWrtModel.parseAddress("ftp://192.168.1.1").valid)
        expect(OpenWrtModel.parseAddress("[fd00::1]").valid)
        for (host in listOf("openwrt", "router.lan", "10.0.0.1", "172.20.1.1", "fe80::1", "::1")) expect(OpenWrtModel.isLocalHost(host), host)
        for (host in listOf("example.com", "172.32.0.1", "2001:db8::1", "1.1.1.1")) expect(!OpenWrtModel.isLocalHost(host), host)
    }

    fun testSectionsAndChanges() {
        val values = mapOf(
            "cfg02" to mapOf(".name" to "cfg02", ".type" to "redirect", ".index" to 2L, ".anonymous" to true, "name" to "Game", "proto" to listOf("tcp", "udp")),
            "wan" to mapOf(".name" to "wan", ".type" to "zone", ".index" to 1L, "masq" to "1"),
        )
        val all = OpenWrtModel.uciSections(values)
        assertEq(listOf("wan", "cfg02"), all.map { it.name })
        expect(all[0].flag("masq"))
        val redirect = OpenWrtModel.uciSections(values, "redirect")[0]
        assertEq("tcp udp", redirect.option("proto"))
        assertEq("redirect \"Game\"", OpenWrtModel.sectionLabel(redirect))
        expect(!redirect.options.containsKey(".type"))
        val changes = mapOf(
            "wireless" to listOf(listOf("set", "default_radio0", "key", "hunter22"), listOf("set", "default_radio0", "ssid", "Home")),
            "firewall" to listOf(listOf("remove", "cfg02"), listOf("add", "cfg09", "redirect")),
        )
        val lines = OpenWrtModel.describeChanges(changes)
        assertEq(4, OpenWrtModel.pendingCount(changes))
        expect("wireless.default_radio0.ssid = Home" in lines)
        expect("firewall.cfg02 deleted" in lines)
        expect("firewall.cfg09: new redirect section" in lines)
        expect(lines.none { it.contains("hunter22") })
    }

    fun testParsers() {
        val leases = OpenWrtModel.parseLeases("1700000600 AA:BB:CC:DD:EE:01 192.168.1.20 laptop 01:aa\n0 aa:bb:cc:dd:ee:02 192.168.1.21 * *\nbroken\n")
        assertEq(2, leases.size)
        assertEq("aa:bb:cc:dd:ee:01", leases[0].mac)
        assertEq("in 10m", OpenWrtModel.leaseExpiry(1_700_000_600, 1_700_000_000))
        assertEq(160L, OpenWrtModel.parseLuciLeases(mapOf("dhcp_leases" to listOf(mapOf("macaddr" to "AA:BB:CC:DD:EE:03", "expires" to 60L))), 100)[0].expires)
        assertEq(listOf("base-files", "luci"), OpenWrtModel.parseInstalledPackages("luci - git-24.1\nbase-files - 1-r1 - The base files\n").map { it.name })
        val status = OpenWrtModel.parseInstalledPackages("Package: dnsmasq\nVersion: 2.90-r3\nStatus: install user installed\n\nPackage: uhttpd\nVersion: 2024.01\n")
        assertEq(listOf("dnsmasq" to "2.90-r3", "uhttpd" to "2024.01"), status.map { it.name to it.version })
        val apk = OpenWrtModel.parseInstalledPackages("luci-app-sqm-1.2.3-r1 noarch {luci} (GPL-2.0) [installed]\nbusybox-1.36.1-r2 x86_64 {busybox}\n")
        assertEq(listOf("busybox", "luci-app-sqm"), apk.map { it.name })
        assertEq("1.2.3-r1", apk[1].version)
        val tools = OpenWrtModel.packageTools
        assertEq(listOf("add", "nano", "htop"), OpenWrtModel.packageArguments(tools[2], "install", listOf("nano", "htop")))
        assertEq(listOf("remove", "nano"), OpenWrtModel.packageArguments(tools[3], "remove", listOf("nano")))
        assertEq(listOf("list-installed"), OpenWrtModel.packageArguments(tools[0], "list"))
        expect(OpenWrtModel.validPackageName("luci-app-sqm"))
        expect(!OpenWrtModel.validPackageName("-force") && !OpenWrtModel.validPackageName("a;reboot"))
        assertEq(listOf("dnsmasq", "uhttpd"), OpenWrtModel.parseServices(mapOf("uhttpd" to mapOf("running" to true), "dnsmasq" to mapOf("enabled" to true))).map { it.name })
        val interfaces = OpenWrtModel.parseInterfaces(Json.parseObject("""{"interface":[{"interface":"wan","up":true,"uptime":3600,"proto":"dhcp","l3_device":"eth1","ipv4-address":[{"address":"203.0.113.7","mask":24}],"route":[{"target":"0.0.0.0","mask":0,"nexthop":"203.0.113.1"}]}]}"""))
        assertEq(listOf("203.0.113.7/24"), interfaces[0].ipv4)
        assertEq("203.0.113.1", interfaces[0].gateway)
        assertEq("1d 1h", OpenWrtModel.formatUptime(90061))
        assertEq("1.5 KB", OpenWrtModel.formatBytes(1536))
        assertEq("1.00 · 0.50", OpenWrtModel.formatLoad(listOf(65536L, 32768L)))
    }

    fun testValidatorsAndEdits() {
        expect(OpenWrtModel.validNetmask("255.255.255.0") && !OpenWrtModel.validNetmask("255.0.255.0"))
        expect(!OpenWrtModel.validIpv4("256.1.1.1"))
        expect(OpenWrtModel.validPort("6000-6010") && !OpenWrtModel.validPort("10-5") && !OpenWrtModel.validPort("70000"))
        expect(OpenWrtModel.wifiKeyProblem("psk2", "short") != null)
        expect(OpenWrtModel.wifiKeyProblem("psk2", "a".repeat(64)) == null)
        expect(OpenWrtModel.wifiKeyProblem("psk2", "z".repeat(64)) != null)
        expect(OpenWrtModel.ssidProblem("x".repeat(33)) != null)
        expect(OpenWrtModel.hostnameProblem("-bad") != null)
        assertEq(95, OpenWrtModel.certificateFingerprint("certificate".toByteArray()).length)
        val lan = UciSection("lan", "interface", options = mapOf("proto" to "static", "ipaddr" to "192.168.1.1", "netmask" to "255.255.255.0", "gateway" to "192.168.1.254"))
        val edit = OpenWrtModel.interfaceEdit(lan, "static", "192.168.2.1", "255.255.255.0", "", "1.1.1.1 9.9.9.9", "", "")
        assertEq("192.168.2.1", edit.values["ipaddr"])
        assertEq(listOf("1.1.1.1", "9.9.9.9"), edit.values["dns"])
        assertEq(listOf("gateway"), edit.removed)
        assertFails<OpenWrtProblem> { OpenWrtModel.interfaceEdit(lan, "static", "x", "", "", "", "", "") }
        assertEq(setOf("ipaddr", "netmask", "gateway"), OpenWrtModel.interfaceEdit(lan, "dhcp", "", "", "", "", "", "").removed.toSet())
        val iface = UciSection("default_radio0", "wifi-iface", options = mapOf("ssid" to "Old", "key" to "oldpassword", "hidden" to "1"))
        val wifi = OpenWrtModel.wifiEdit(iface, "Home", "none", "", "lan", false, false, false)
        assertEq("1", wifi.values["disabled"])
        assertEq(setOf("key", "hidden"), wifi.removed.toSet())
        val forward = OpenWrtModel.forwardEdit(null, "Game", "tcp udp", "8080", "192.168.1.20", "", true)
        assertEq("DNAT", forward.values["target"])
        assertEq(listOf("tcp", "udp"), forward.values["proto"])
        expect(forward.removed.isEmpty())
        assertFails<OpenWrtProblem> { OpenWrtModel.reservationValues("", "nope", "192.168.1.2") }
        assertEq("device-ddeeff", OpenWrtModel.reservationValues("", "AA:BB:CC:DD:EE:FF", "192.168.1.2")["name"])
        val radio = OpenWrtModel.radioEdit(UciSection("radio0", "wifi-device", options = mapOf("disabled" to "1")), true, "6", "de", "")
        assertEq("DE", radio.values["country"])
        assertEq(listOf("disabled"), radio.removed)
    }

    fun testStorage() {
        val blocks = OpenWrtModel.parseBlockDevices(mapOf(
            "sda1" to mapOf("dev" to "/dev/sda1", "uuid" to "1234-ABCD", "label" to "Media Disk", "type" to "exfat", "size" to 64_000_000_000L, "mount" to "/mnt/sda1"),
            "sdb1" to mapOf("dev" to "/dev/sdb1", "uuid" to "aa-bb", "type" to "ext4", "size" to 1000L),
            "mtdblock5" to mapOf("dev" to "/dev/mtdblock5"),
            "ubiblock0_1" to mapOf("dev" to "/dev/ubiblock0_1"),
        ))
        assertEq(listOf("sda1", "sdb1"), blocks.map { it.name })
        for (name in listOf("mmcblk0p1", "nvme0n1p2", "sdb", "sdaa3")) expect(OpenWrtModel.isStorageDevice(name), name)
        for (name in listOf("zram0", "loop0", "mtdblock2", "sd1")) expect(!OpenWrtModel.isStorageDevice(name), name)
        val mounts = OpenWrtModel.parseMountPoints(mapOf("result" to listOf(mapOf("device" to "/dev/sda1", "mount" to "/mnt/sda1", "size" to 64_000_000_000L, "avail" to 16_000_000_000L))))
        val fstab = OpenWrtModel.uciSections(mapOf("cfg02" to mapOf(".name" to "cfg02", ".type" to "mount", ".index" to 1L, "uuid" to "aa-bb", "target" to "/mnt/backup", "enabled" to "0")))
        val disks = OpenWrtModel.storageDisks(blocks, mounts, fstab)
        expect(disks[0].mounted && disks[0].usageKnown && disks[0].avail == 16_000_000_000L)
        assertEq("cfg02", disks[1].fstabSection)
        expect(!disks[1].bootEnabled)
        assertEq("Media Disk (sda1)", disks[0].title)
        assertEq("/mnt/Media_Disk", OpenWrtModel.suggestedMountPath(blocks[0]))
        expect(OpenWrtModel.mountPathProblem("/mnt/usb") == null)
        for (bad in listOf("/etc", "/mnt/../etc", "/mnt/a b", "/mnt/", "mnt/usb")) expect(OpenWrtModel.mountPathProblem(bad) != null, bad)
        assertEq("1234-ABCD", OpenWrtModel.mountSectionValues(blocks[0], "/mnt/media")["uuid"])
        assertEq("samba4", OpenWrtModel.shareConfig(listOf("network", "samba4")))
        assertEq("share", OpenWrtModel.shareSectionType("ksmbd"))
        expect(OpenWrtModel.shareNameProblem("Media") == null && OpenWrtModel.shareNameProblem("a/b") != null)
        expect("block-mount" in OpenWrtModel.usbStoragePackages)
    }

    fun testStorageCheck() {
        assertEq(listOf("sda", "sda1", "mmcblk0"), OpenWrtModel.parsePartitions("major minor  #blocks  name\n\n  31 0 1024 mtdblock0\n 8 0 100 sda\n 8 1 99 sda1\n 179 0 50 mmcblk0\n"))
        val entries = listOf("usb1", "usb2", "1-1", "1-1:1.0", "2-1.4")
        assertEq(2, OpenWrtModel.usbControllerCount(entries))
        assertEq(listOf("1-1", "2-1.4"), OpenWrtModel.usbDeviceEntries(entries))
        assertEq(
            listOf("[1.0] usb 1-1: new device", "[2.0] sd 0:0:0:0: [sda] No Caching mode page found"),
            OpenWrtModel.storageKernelLines("<6>[1.0] usb 1-1: new device\n<6>[1.1] eth0: link up\n<3>[2.0] sd 0:0:0:0: [sda] No Caching mode page found\n"),
        )
        assertEq(OpenWrtModel.usbStoragePackages.size - 2, OpenWrtModel.missingStoragePackages(listOf("block-mount", "kmod-usb-storage")).size)
        expect(OpenWrtModel.fstabFromDetect("config 'global'\n\toption\tanon_mount\t'0'").endsWith("'0'\n"))
        expect(OpenWrtModel.fstabFromDetect("").startsWith("config global\n"))
        assertEq(listOf("b", "c"), OpenWrtModel.outputTail("a\n\nb\nc\n", 2))

        val empty = OpenWrtStorageCheck(packagesKnown = true, usbKnown = true, usbControllers = 1, partitionsKnown = true)
        expect(OpenWrtModel.storageFindings(empty)[0].contains("sees nothing plugged into its USB port"))
        expect(OpenWrtModel.storageFindings(empty.copy(usbControllers = 0))[0].contains("no USB controller driver"))
        val noDriver = empty.copy(usbDevices = listOf("Flash Disk (1-1)"))
        expect(OpenWrtModel.storageFindings(noDriver)[0].contains("kmod-usb-storage"))
        val noBlock = noDriver.copy(installed = listOf("kmod-usb-storage"), partitions = listOf("sda", "sda1"))
        expect(OpenWrtModel.storageFindings(noBlock)[0].contains("block-mount is not installed"))
        val noFstab = noBlock.copy(blockTool = true, disks = 1)
        val findings = OpenWrtModel.storageFindings(noFstab)
        expect(findings.joinToString("\n").contains("/etc/config/fstab"))
        expect(findings.last().contains("Turn on Mount any disk"))
        val ready = noFstab.copy(fstabExists = true, installed = OpenWrtModel.usbStoragePackages, mounted = 1)
        assertEq(listOf("The disk is mounted."), OpenWrtModel.storageFindings(ready))
        expect(OpenWrtModel.storageReport(ready).startsWith("• The disk is mounted."))
        expect(OpenWrtModel.storageReport(ready).contains("Storage packages: all installed"))
    }

    fun testMissingFstabIsCreatedAndStorageChecked() {
        val router = FakeRouter()
        router.fstabMissing = true
        val client = OpenWrtClient(router, "http://192.168.1.1")
        client.signIn("root", "secret")
        val snapshot = client.loadSnapshot(now = 0)
        expect(snapshot.fstabMissing)
        expect(snapshot.problems.isEmpty(), snapshot.problems.toString())
        expect(client.createFstab().ok)
        @Suppress("UNCHECKED_CAST")
        val written = router.last("file", "write")?.get(3) as JsonMap
        assertEq("/etc/config/fstab", written.str("path"))
        expect(written.str("data").startsWith("config 'global'"))
        val (check, fresh) = client.checkStorage(snapshot)
        expect(!fresh.fstabMissing)
        expect(check.fstabExists && check.packagesKnown && check.blockTool)
        assertEq(1, check.usbControllers)
        assertEq(listOf("Flash Disk (1-1)"), check.usbDevices)
        assertEq(listOf("sda", "sda1"), check.partitions)
        assertEq(1, check.disks)
        assertEq(1, check.mounted)
        assertEq(listOf("[    5.1] usb 1-1: new high-speed USB device number 2"), check.kernelLines)
        expect(OpenWrtModel.storageReport(check).contains("USB devices: Flash Disk (1-1)"))
    }

    fun testRenewedSessionSeesNewlyInstalledShareConfig() {
        val router = FakeRouter()
        val client = OpenWrtClient(router, "http://192.168.1.1")
        expect(client.signIn("root", "secret").ok)
        router.shareAclFromLogin = router.logins + 1
        val stale = client.loadSnapshot(now = 0)
        assertEq("", stale.shareConfig)
        expect(!OpenWrtModel.needsShareSessionRenewal(stale))
        val installed = stale.copy(packages = listOf(OpenWrtPackage("ksmbd-server", "1"), OpenWrtPackage("luci-app-ksmbd", "1")))
        expect(OpenWrtModel.needsShareSessionRenewal(installed))
        expect(!OpenWrtModel.needsShareSessionRenewal(installed.copy(changes = mapOf("network" to listOf(listOf("set", "lan", "dns", "1.1.1.1"))))))
        val before = router.logins
        expect(client.renewSession().ok)
        assertEq(before + 1, router.logins)
        val fresh = client.loadSnapshot(now = 0)
        assertEq("ksmbd", fresh.shareConfig)
        assertEq("Media", fresh.shares.firstOrNull()?.name)
        client.signOut()
        val signedOut = router.logins
        expect(!client.renewSession().ok)
        assertEq(signedOut, router.logins)
    }

    fun testMissingShareConfigIsCreatedOnlyWhenReadable() {
        val router = FakeRouter()
        router.ksmbdMissing = true
        val client = OpenWrtClient(router, "http://192.168.1.1")
        expect(client.signIn("root", "secret").ok)
        router.shareAclFromLogin = Int.MAX_VALUE
        val denied = client.createShareConfig()
        expect(!denied.ok && "Sign out and sign in again" in denied.error)
        expect(router.last("file", "write") == null)
        router.shareAclFromLogin = 0
        assertEq("", client.loadSnapshot(now = 0).shareConfig)
        expect(client.createShareConfig().ok)
        val written = router.last("file", "write")?.getOrNull(3) as? Map<*, *>
        assertEq("/etc/config/ksmbd", written?.get("path"))
        assertEq(384L, (written?.get("mode") as? Number)?.toLong())
        assertEq("ksmbd", client.loadSnapshot(now = 0).shareConfig)
    }

    fun testShareConfigMissingFromConfigListIsReadDirectly() {
        assertEq(listOf("ksmbd"), OpenWrtModel.shareConfigCandidates(listOf("luci-app-ksmbd")))
        assertEq(listOf("samba4"), OpenWrtModel.shareConfigCandidates(listOf("samba4-server")))
        assertEq(emptyList<String>(), OpenWrtModel.shareConfigCandidates(listOf("luci")))
        val router = FakeRouter()
        router.configListHidesKsmbd = true
        router.installed += listOf("ksmbd-server", "luci-app-ksmbd")
        val client = OpenWrtClient(router, "http://192.168.1.1")
        expect(client.signIn("root", "secret").ok)
        val snapshot = client.loadSnapshot(now = 0)
        expect(snapshot.configNames.isNotEmpty())
        assertEq("ksmbd", snapshot.shareConfig)
        assertEq("Media", snapshot.shares.firstOrNull()?.name)
        expect(!OpenWrtModel.needsShareSessionRenewal(snapshot))
    }

    fun testClientSignInRenewalAndExec() {
        val router = FakeRouter()
        val client = OpenWrtClient(router, "http://192.168.1.1")
        val wrong = client.signIn("root", "guess")
        expect(!wrong.ok && wrong.error.contains("password"))
        expect(client.signIn("root", "secret").ok)
        router.expireNextBoard = true
        val board = client.call("system", "board")
        expect(board.ok)
        assertEq("Test Router", board.data.str("model"))
        assertEq(2, router.logins)
        assertEq(1, client.renewals)
        assertEq("sess2", router.last("system", "board")?.get(0))
        val failed = client.exec("/bin/false", emptyList())
        expect(!failed.ok)
        assertEq("nope", failed.error)
    }

    fun testClientUnreachableAndCertificate() {
        val gone = OpenWrtClient({ _, _, _, _ -> throw IOException("connection refused") }, "http://192.168.1.1").signIn("root", "secret")
        expect(gone.unreachable && gone.error.contains("192.168.1.1"))
        val untrusted = OpenWrtClient({ _, _, _, _ -> throw OpenWrtCertificateException("AA:BB") }, "https://192.168.1.1").signIn("root", "secret")
        assertEq("AA:BB", untrusted.certificate)
    }

    fun testSnapshotStageAndApply() {
        val router = FakeRouter()
        val client = OpenWrtClient(router, "http://192.168.1.1")
        client.signIn("root", "secret")
        val snapshot = client.loadSnapshot(now = 0)
        assertEq(listOf("lan", "wan"), snapshot.interfaceSections.map { it.name })
        assertEq("OpenWrt", snapshot.hostname)
        assertEq(listOf("base-files", "luci"), snapshot.packages.map { it.name })
        assertEq("/bin/opkg", snapshot.packageTool?.program)
        assertEq(listOf("sda1"), snapshot.disks.map { it.block.name })
        expect(snapshot.autoMount)
        assertEq("Media", snapshot.shares.firstOrNull()?.name)

        expect(client.stage("network", "lan", UciEdit(mapOf("dns" to listOf("1.1.1.1")), listOf("gateway"))).ok)
        @Suppress("UNCHECKED_CAST")
        assertEq("lan", (router.last("uci", "set")?.get(3) as JsonMap).str("section"))
        @Suppress("UNCHECKED_CAST")
        assertEq(listOf("gateway"), (router.last("uci", "delete")?.get(3) as JsonMap)["options"])
        expect(router.last("uci", "apply") == null)

        router.confirmFailures = 2
        val waits = mutableListOf<Double>()
        expect(client.apply(true) { waits += it } == OpenWrtApplyOutcome.Confirmed)
        assertEq(listOf(3.0, 2.0, 2.0), waits)
        @Suppress("UNCHECKED_CAST")
        assertEq(true, (router.last("uci", "apply")?.get(3) as JsonMap)["rollback"])
        router.confirmFailures = 1000
        expect(client.apply(true) {} is OpenWrtApplyOutcome.RolledBack)
        expect(client.apply(false) { throw AssertionError("no waiting without rollback") } == OpenWrtApplyOutcome.Applied)
        expect(!client.packageAction(OpenWrtModel.packageTools[3], "install", listOf("a;reboot")).ok)
        expect(client.packageAction(OpenWrtModel.packageTools[3], "install", listOf("luci-app-ksmbd")).ok)
        assertEq(listOf("install luci-app-ksmbd"), router.calls.filter { it.getOrNull(2) == "exec" }.mapNotNull { (it.getOrNull(3) as? Map<*, *>)?.get("params") as? List<*> }
            .map { it.joinToString(" ") }.filter { it.startsWith("install") })
    }

    fun testRouterHttpSpeaksHttpOverASocket() {
        val server = ServerSocket(0)
        var request = ""
        val worker = thread {
            server.accept().use { socket ->
                val input = socket.getInputStream()
                val bytes = StringBuilder()
                while (!bytes.contains("\r\n\r\n")) bytes.append(input.read().toChar())
                val length = Regex("Content-Length: (\\d+)").find(bytes)!!.groupValues[1].toInt()
                repeat(length) { bytes.append(input.read().toChar()) }
                request = bytes.toString()
                socket.getOutputStream().write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{\"re\r\n11\r\nsult\":[0,{\"a\":1}]\r\n1\r\n}\r\n0\r\n\r\n".toByteArray(),
                )
            }
        }
        val response = RouterHttp("").post("http://127.0.0.1:${server.localPort}/ubus", "application/json", "{\"x\":1}".toByteArray(), 5_000)
        worker.join()
        server.close()
        expect(request.startsWith("POST /ubus HTTP/1.0\r\n"), request)
        expect(request.endsWith("{\"x\":1}"), request)
        assertEq(200, response.status)
        assertEq(1L, OpenWrtModel.parseReply(response.body, response.status).data["a"])
    }
}
