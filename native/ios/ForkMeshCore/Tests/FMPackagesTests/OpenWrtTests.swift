import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMPackages

final class FakeRouterState: @unchecked Sendable {
    private let lock = NSLock()
    private var sessionValue = ""
    private var loginCount = 0
    private var expireNextBoard = false
    private var calls: [[JSONValue]] = []
    private var confirmFailures = 0
    private var fstabMissing = false
    private var ksmbdMissing = false
    private var shareAclFromLogin = 0

    var logins: Int { lock.withLock { loginCount } }
    var session: String { lock.withLock { sessionValue } }

    func expireOnce() { lock.withLock { expireNextBoard = true } }
    func failConfirm(times: Int) { lock.withLock { confirmFailures = times } }
    func removeFstab() { lock.withLock { fstabMissing = true } }
    func removeKsmbdConfig() { lock.withLock { ksmbdMissing = true } }
    func grantShareAclOnNextLogin() { lock.withLock { shareAclFromLogin = loginCount + 1 } }
    func setShareAcl(_ granted: Bool) { lock.withLock { shareAclFromLogin = granted ? 0 : Int.max } }

    func last(_ object: String, _ method: String) -> [JSONValue]? {
        lock.withLock { calls.last { $0.count > 2 && $0[1].text == object && $0[2].text == method } }
    }

    func handle(_ request: HTTPRequest) throws -> HTTPResponse {
        let body = JSON.parseObject(request.body ?? Data())
        let params = body.list("params")
        lock.withLock { calls.append(params) }
        let sid = params.first?.text ?? ""
        let object = params.count > 1 ? params[1].text : ""
        let method = params.count > 2 ? params[2].text : ""
        let args = params.count > 3 ? params[3].objectOrEmpty : JSONObject()
        func reply(_ result: JSONValue) -> HTTPResponse {
            HTTPResponse(json: ["jsonrpc": "2.0", "id": body["id"] ?? 1, "result": result])
        }
        let denied = HTTPResponse(json: ["jsonrpc": "2.0", "id": body["id"] ?? 1, "error": ["code": -32002, "message": "Access denied"]])
        if object == "session" && method == "login" {
            guard sid == OpenWrt.anonymousSession, args.str("password") == "secret" else { return reply([6]) }
            let fresh: String = lock.withLock {
                loginCount += 1
                sessionValue = "sess\(loginCount)"
                return sessionValue
            }
            return reply([0, ["ubus_rpc_session": .string(fresh)]])
        }
        if sid != session { return denied }
        if object == "system" && method == "board" {
            let expire: Bool = lock.withLock {
                let value = expireNextBoard
                expireNextBoard = false
                if value { sessionValue = "expired" }
                return value
            }
            if expire { return denied }
            return reply([0, ["hostname": "OpenWrt", "model": "Test Router"]])
        }
        if object == "uci" && method == "confirm" {
            let fail: Bool = lock.withLock {
                guard confirmFailures > 0 else { return false }
                confirmFailures -= 1
                return true
            }
            if fail { throw HTTPError.transport("connection refused") }
            return reply([0])
        }
        if object == "uci" && method == "configs" {
            let shared = lock.withLock { loginCount >= shareAclFromLogin && !ksmbdMissing }
            return reply([0, ["configs": .array((["network", "fstab"] + (shared ? ["ksmbd"] : [])).map { .string($0) })]])
        }
        if object == "uci" && method == "get" {
            switch args.str("config") {
            case "network":
                return reply([0, ["values": [
                    "lan": [".name": "lan", ".type": "interface", ".index": 1, "proto": "static", "ipaddr": "192.168.1.1", "netmask": "255.255.255.0"],
                    "wan": [".name": "wan", ".type": "interface", ".index": 2, "proto": "dhcp"],
                ]]])
            case "fstab":
                if lock.withLock({ fstabMissing }) { return reply([4]) }
                return reply([0, ["values": ["cfg01": [".name": "cfg01", ".type": "global", ".anonymous": true, ".index": 0, "anon_mount": "1"]]]])
            case "ksmbd":
                let (allowed, missing) = lock.withLock { (loginCount >= shareAclFromLogin, ksmbdMissing) }
                if !allowed { return reply([6]) }
                if missing { return reply([4]) }
                return reply([0, ["values": ["cfg02": [".name": "cfg02", ".type": "share", ".index": 0, "name": "Media", "path": "/mnt/sda1", "guest_ok": "yes"]]]])
            default:
                return reply([4])
            }
        }
        if object == "luci" && method == "getBlockDevices" {
            return reply([0, ["sda1": ["dev": "/dev/sda1", "uuid": "1234", "label": "Media", "type": "exfat", "size": 64_000_000_000, "mount": "/mnt/sda1"],
                              "mtdblock3": ["dev": "/dev/mtdblock3", "type": "squashfs"]]])
        }
        if object == "luci" && method == "getMountPoints" {
            return reply([0, ["result": [["device": "/dev/sda1", "mount": "/mnt/sda1", "type": "exfat", "size": 64_000_000_000, "avail": 1_000_000_000]]]])
        }
        if object == "file" && method == "exec" {
            switch args.str("command") {
            case "/bin/false": return reply([0, ["code": 1, "stderr": "nope"]])
            case "/bin/opkg": return reply([0, ["code": 0, "stdout": "base-files - 1-r1\nluci - git-24\n"]])
            case "/sbin/block": return reply([0, ["code": 0, "stdout": "config 'global'\n\toption\tanon_mount\t'0'\n"]])
            case "/bin/dmesg": return reply([0, ["code": 0, "stdout": "<6>[    5.1] usb 1-1: new high-speed USB device number 2\n<5>[    6.0] random: crng init done\n"]])
            default: return reply([6])
            }
        }
        if object == "file" && method == "write" && args.str("path") == "/etc/config/ksmbd" {
            guard args.str("data").hasPrefix("config globals\n") else { return reply([6]) }
            lock.withLock { ksmbdMissing = false }
            return reply([0])
        }
        if object == "file" && method == "write" {
            guard args.str("path") == "/etc/config/fstab", args.str("data").contains("global") else { return reply([6]) }
            lock.withLock { fstabMissing = false }
            return reply([0])
        }
        if object == "file" && method == "list" && args.str("path") == "/sys/bus/usb/devices" {
            return reply([0, ["entries": [["name": "usb1"], ["name": "1-1"], ["name": "1-1:1.0"]]]])
        }
        if object == "file" && method == "read" && args.str("path") == "/sys/bus/usb/devices/1-1/product" { return reply([0, ["data": "Flash Disk\n"]]) }
        if object == "file" && method == "read" && args.str("path") == "/proc/partitions" {
            return reply([0, ["data": "major minor  #blocks  name\n\n  31        0       1024 mtdblock0\n   8        0   64000000 sda\n   8        1   63999000 sda1\n"]])
        }
        if object == "uci" && method == "changes" { return reply([0, ["changes": .object(JSONObject())]]) }
        return reply([0])
    }
}

final class OpenWrtModelTests: XCTestCase {
    func testCallBodyAndReplies() {
        let body = JSON.parseObject(OpenWrtModel.callBody(id: 7, session: "", object: "uci", method: "get", args: ["config": "network"]))
        XCTAssertEqual(body.str("method"), "call")
        XCTAssertEqual(body.int("id"), 7)
        XCTAssertEqual(body.list("params").first?.text, OpenWrt.anonymousSession)
        XCTAssertEqual(body.list("params")[3].str("config"), "network")

        let ok = OpenWrtModel.parseReply(Data(#"{"jsonrpc":"2.0","id":1,"result":[0,{"a":1}]}"#.utf8))
        XCTAssertTrue(ok.ok)
        XCTAssertEqual(ok.data.int("a"), 1)
        XCTAssertTrue(OpenWrtModel.parseReply(Data(#"{"result":[6]}"#.utf8)).accessDenied)
        XCTAssertTrue(OpenWrtModel.parseReply(Data(#"{"error":{"code":-32002,"message":"Access denied"}}"#.utf8)).accessDenied)
        XCTAssertTrue(OpenWrtModel.parseReply(Data(#"{"error":{"code":-32000}}"#.utf8)).error.contains("not found"))
        let slow = OpenWrtModel.parseReply(Data(#"{"error":{"code":-32003,"message":"ubus request timed out"}}"#.utf8))
        XCTAssertTrue(slow.timedOut && !slow.ok && !slow.accessDenied)
        XCTAssertTrue(OpenWrtModel.parseReply(Data(#"{"result":[7]}"#.utf8)).timedOut)
        let listed = [OpenWrtPackage(name: "ksmbd-server", version: "1"), OpenWrtPackage(name: "luci-app-ksmbd", version: "1")]
        XCTAssertTrue(OpenWrtModel.packagesInstalled(listed, ["luci-app-ksmbd"]))
        XCTAssertFalse(OpenWrtModel.packagesInstalled(listed, ["luci-app-ksmbd", "nano"]))
        XCTAssertTrue(OpenWrtModel.parseReply(Data("<html>".utf8), httpStatus: 404).error.contains("/ubus"))
    }

    func testAddresses() {
        let plain = OpenWrtModel.parseAddress("192.168.1.1")
        XCTAssertTrue(plain.valid)
        XCTAssertEqual(plain.base?.absoluteString, "http://192.168.1.1")
        let tls = OpenWrtModel.parseAddress("https://router.lan:8443/cgi-bin/luci")
        XCTAssertTrue(tls.valid && tls.encrypted)
        XCTAssertEqual(tls.base?.port, 8443)
        XCTAssertEqual(tls.base?.path, "")
        XCTAssertFalse(OpenWrtModel.parseAddress("8.8.8.8").valid)
        XCTAssertFalse(OpenWrtModel.parseAddress("http://root:pw@192.168.1.1").valid)
        XCTAssertFalse(OpenWrtModel.parseAddress("ftp://192.168.1.1").valid)
        XCTAssertTrue(OpenWrtModel.parseAddress("[fd00::1]").valid)
        for host in ["openwrt", "router.lan", "10.0.0.1", "172.20.1.1", "fe80::1", "::1"] { XCTAssertTrue(OpenWrtModel.isLocalHost(host), host) }
        for host in ["example.com", "172.32.0.1", "2001:db8::1", "1.1.1.1"] { XCTAssertFalse(OpenWrtModel.isLocalHost(host), host) }
    }

    func testSectionsAndChanges() {
        let values: JSONObject = [
            "cfg02": [".name": "cfg02", ".type": "redirect", ".index": 2, ".anonymous": true, "name": "Game", "proto": ["tcp", "udp"]],
            "wan": [".name": "wan", ".type": "zone", ".index": 1, "masq": "1"],
        ]
        let all = OpenWrtModel.uciSections(values)
        XCTAssertEqual(all.map(\.name), ["wan", "cfg02"])
        XCTAssertTrue(all[0].flag("masq"))
        let redirect = OpenWrtModel.uciSections(values, type: "redirect")[0]
        XCTAssertEqual(redirect.option("proto"), "tcp udp")
        XCTAssertEqual(OpenWrtModel.sectionLabel(redirect), "redirect \"Game\"")
        XCTAssertFalse(redirect.options.contains(".type"))

        let changes: JSONObject = [
            "wireless": [["set", "default_radio0", "key", "hunter22"], ["set", "default_radio0", "ssid", "Home"]],
            "firewall": [["remove", "cfg02"], ["add", "cfg09", "redirect"]],
        ]
        let lines = OpenWrtModel.describeChanges(changes)
        XCTAssertEqual(OpenWrtModel.pendingCount(changes), 4)
        XCTAssertTrue(lines.contains("wireless.default_radio0.ssid = Home"))
        XCTAssertTrue(lines.contains("firewall.cfg02 deleted"))
        XCTAssertTrue(lines.contains("firewall.cfg09: new redirect section"))
        XCTAssertFalse(lines.joined().contains("hunter22"))
    }

    func testParsers() {
        let leases = OpenWrtModel.parseLeases("1700000600 AA:BB:CC:DD:EE:01 192.168.1.20 laptop 01:aa\n0 aa:bb:cc:dd:ee:02 192.168.1.21 * *\nbroken\n")
        XCTAssertEqual(leases.count, 2)
        XCTAssertEqual(leases[0].mac, "aa:bb:cc:dd:ee:01")
        XCTAssertEqual(OpenWrtModel.leaseExpiry(1_700_000_600, now: 1_700_000_000), "in 10m")
        XCTAssertEqual(OpenWrtModel.parseLuciLeases(["dhcp_leases": [["macaddr": "AA:BB:CC:DD:EE:03", "ipaddr": "192.168.1.5", "expires": 60]]], now: 100)[0].expires, 160)

        let opkg = OpenWrtModel.parseInstalledPackages("luci - git-24.1\nbase-files - 1-r1 - The base files\n")
        XCTAssertEqual(opkg.map(\.name), ["base-files", "luci"])
        XCTAssertEqual(opkg[0].version, "1-r1")
        let status = OpenWrtModel.parseInstalledPackages("Package: dnsmasq\nVersion: 2.90-r3\nStatus: install user installed\n\nPackage: uhttpd\nVersion: 2024.01\n")
        XCTAssertEqual(status.map(\.name), ["dnsmasq", "uhttpd"])
        let apk = OpenWrtModel.parseInstalledPackages("luci-app-sqm-1.2.3-r1 noarch {luci} (GPL-2.0) [installed]\nbusybox-1.36.1-r2 x86_64 {busybox}\n")
        XCTAssertEqual(apk.map(\.name), ["busybox", "luci-app-sqm"])
        XCTAssertEqual(apk[1].version, "1.2.3-r1")
        let tools = OpenWrtModel.packageTools
        XCTAssertEqual(OpenWrtModel.packageArguments(tools[2], action: "install", names: ["nano", "htop"]), ["add", "nano", "htop"])
        XCTAssertEqual(OpenWrtModel.packageArguments(tools[3], action: "remove", names: ["nano"]), ["remove", "nano"])
        XCTAssertEqual(OpenWrtModel.packageArguments(tools[0], action: "list"), ["list-installed"])
        XCTAssertTrue(OpenWrtModel.validPackageName("luci-app-sqm"))
        XCTAssertFalse(OpenWrtModel.validPackageName("-force"))
        XCTAssertFalse(OpenWrtModel.validPackageName("a;reboot"))

        let services = OpenWrtModel.parseServices(["uhttpd": ["enabled": true, "running": true], "dnsmasq": ["enabled": true, "running": false]])
        XCTAssertEqual(services.map(\.name), ["dnsmasq", "uhttpd"])
        let interfaces = OpenWrtModel.parseInterfaces(JSON.parseObject(#"{"interface":[{"interface":"wan","up":true,"uptime":3600,"proto":"dhcp","l3_device":"eth1","ipv4-address":[{"address":"203.0.113.7","mask":24}],"route":[{"target":"0.0.0.0","mask":0,"nexthop":"203.0.113.1"}]}]}"#))
        XCTAssertEqual(interfaces[0].ipv4, ["203.0.113.7/24"])
        XCTAssertEqual(interfaces[0].gateway, "203.0.113.1")
        XCTAssertEqual(OpenWrtModel.formatUptime(90061), "1d 1h")
        XCTAssertEqual(OpenWrtModel.formatBytes(1536), "1.5 KB")
        XCTAssertEqual(OpenWrtModel.formatLoad([65536, 32768]), "1.00 · 0.50")
    }

    func testValidatorsAndEdits() throws {
        XCTAssertTrue(OpenWrtModel.validNetmask("255.255.255.0"))
        XCTAssertFalse(OpenWrtModel.validNetmask("255.0.255.0"))
        XCTAssertFalse(OpenWrtModel.validIpv4("256.1.1.1"))
        XCTAssertTrue(OpenWrtModel.validPort("6000-6010"))
        XCTAssertFalse(OpenWrtModel.validPort("10-5"))
        XCTAssertFalse(OpenWrtModel.validPort("70000"))
        XCTAssertNotNil(OpenWrtModel.wifiKeyProblem(encryption: "psk2", key: "short"))
        XCTAssertNil(OpenWrtModel.wifiKeyProblem(encryption: "psk2", key: String(repeating: "a", count: 64)))
        XCTAssertNotNil(OpenWrtModel.wifiKeyProblem(encryption: "psk2", key: String(repeating: "z", count: 64)))
        XCTAssertNotNil(OpenWrtModel.ssidProblem(String(repeating: "x", count: 33)))
        XCTAssertNotNil(OpenWrtModel.hostnameProblem("-bad"))
        XCTAssertEqual(OpenWrtModel.certificateFingerprint(Array("certificate".utf8)).count, 95)

        let lan = UciSection(name: "lan", type: "interface", options: ["proto": "static", "ipaddr": "192.168.1.1", "netmask": "255.255.255.0", "gateway": "192.168.1.254"])
        let edit = try OpenWrtModel.interfaceEdit(lan, proto: "static", ipaddr: "192.168.2.1", netmask: "255.255.255.0", gateway: "", dns: "1.1.1.1 9.9.9.9",
                                                  pppoeUser: "", pppoePassword: "")
        XCTAssertEqual(edit.values["ipaddr"], "192.168.2.1")
        XCTAssertEqual(edit.values["dns"], ["1.1.1.1", "9.9.9.9"])
        XCTAssertEqual(edit.removed, ["gateway"])
        XCTAssertThrowsError(try OpenWrtModel.interfaceEdit(lan, proto: "static", ipaddr: "x", netmask: "", gateway: "", dns: "", pppoeUser: "", pppoePassword: ""))
        let dhcp = try OpenWrtModel.interfaceEdit(lan, proto: "dhcp", ipaddr: "", netmask: "", gateway: "", dns: "", pppoeUser: "", pppoePassword: "")
        XCTAssertEqual(Set(dhcp.removed), ["ipaddr", "netmask", "gateway"])

        let iface = UciSection(name: "default_radio0", type: "wifi-iface", options: ["ssid": "Old", "key": "oldpassword", "hidden": "1"])
        let wifi = try OpenWrtModel.wifiEdit(iface, ssid: "Home", encryption: "none", key: "", network: "lan", enabled: false, hidden: false, isolate: false)
        XCTAssertEqual(wifi.values["disabled"], "1")
        XCTAssertEqual(Set(wifi.removed), ["key", "hidden"])
        let forward = try OpenWrtModel.forwardEdit(nil, name: "Game", proto: "tcp udp", externalPort: "8080", ip: "192.168.1.20", internalPort: "", enabled: true)
        XCTAssertEqual(forward.values["target"], "DNAT")
        XCTAssertEqual(forward.values["proto"], ["tcp", "udp"])
        XCTAssertTrue(forward.removed.isEmpty)
        XCTAssertThrowsError(try OpenWrtModel.reservationValues(name: "", mac: "nope", ip: "192.168.1.2"))
        XCTAssertEqual(try OpenWrtModel.reservationValues(name: "", mac: "AA:BB:CC:DD:EE:FF", ip: "192.168.1.2").str("name"), "device-ddeeff")
        let radio = try OpenWrtModel.radioEdit(UciSection(name: "radio0", type: "wifi-device", options: ["disabled": "1"]), enabled: true, channel: "6", country: "de", txpower: "")
        XCTAssertEqual(radio.values["country"], "DE")
        XCTAssertEqual(radio.removed, ["disabled"])
    }

    func testStorage() {
        let blocks = OpenWrtModel.parseBlockDevices([
            "sda1": ["dev": "/dev/sda1", "uuid": "1234-ABCD", "label": "Media Disk", "type": "exfat", "size": 64_000_000_000, "mount": "/mnt/sda1"],
            "sdb1": ["dev": "/dev/sdb1", "uuid": "aa-bb", "type": "ext4", "size": 1000],
            "mtdblock5": ["dev": "/dev/mtdblock5"],
            "ubiblock0_1": ["dev": "/dev/ubiblock0_1"],
        ])
        XCTAssertEqual(blocks.map(\.name), ["sda1", "sdb1"])
        for name in ["mmcblk0p1", "nvme0n1p2", "sdb", "sdaa3"] { XCTAssertTrue(OpenWrtModel.isStorageDevice(name), name) }
        for name in ["zram0", "loop0", "mtdblock2", "sd1"] { XCTAssertFalse(OpenWrtModel.isStorageDevice(name), name) }
        let mounts = OpenWrtModel.parseMountPoints(["result": [["device": "/dev/sda1", "mount": "/mnt/sda1", "size": 64_000_000_000, "avail": 16_000_000_000]]])
        let fstab = OpenWrtModel.uciSections([
            "cfg02": [".name": "cfg02", ".type": "mount", ".index": 1, "uuid": "aa-bb", "target": "/mnt/backup", "enabled": "0"],
        ])
        let disks = OpenWrtModel.storageDisks(blocks, mounts: mounts, fstab: fstab)
        XCTAssertTrue(disks[0].mounted && disks[0].usageKnown)
        XCTAssertEqual(disks[0].avail, 16_000_000_000)
        XCTAssertEqual(disks[1].fstabSection, "cfg02")
        XCTAssertFalse(disks[1].bootEnabled)
        XCTAssertEqual(disks[0].title, "Media Disk (sda1)")
        XCTAssertEqual(OpenWrtModel.suggestedMountPath(blocks[0]), "/mnt/Media_Disk")
        XCTAssertNil(OpenWrtModel.mountPathProblem("/mnt/usb"))
        for bad in ["/etc", "/mnt/../etc", "/mnt/a b", "/mnt/", "mnt/usb"] { XCTAssertNotNil(OpenWrtModel.mountPathProblem(bad), bad) }
        XCTAssertEqual(OpenWrtModel.mountSectionValues(blocks[0], target: "/mnt/media").str("uuid"), "1234-ABCD")
        XCTAssertEqual(OpenWrtModel.shareConfig(["network", "samba4"]), "samba4")
        XCTAssertEqual(OpenWrtModel.shareSectionType("ksmbd"), "share")
        XCTAssertNil(OpenWrtModel.shareNameProblem("Media"))
        XCTAssertNotNil(OpenWrtModel.shareNameProblem("a/b"))
        XCTAssertTrue(OpenWrtModel.usbStoragePackages.contains("block-mount"))
        XCTAssertEqual(OpenWrtModel.shareSupportPackages, ["ksmbd-server", "luci-app-ksmbd"])
    }

    func testStorageCheck() {
        XCTAssertEqual(OpenWrtModel.parsePartitions("major minor  #blocks  name\n\n  31 0 1024 mtdblock0\n 8 0 100 sda\n 8 1 99 sda1\n 179 0 50 mmcblk0\n"), ["sda", "sda1", "mmcblk0"])
        let entries = ["usb1", "usb2", "1-1", "1-1:1.0", "2-1.4"]
        XCTAssertEqual(OpenWrtModel.usbControllerCount(entries), 2)
        XCTAssertEqual(OpenWrtModel.usbDeviceEntries(entries), ["1-1", "2-1.4"])
        XCTAssertEqual(OpenWrtModel.storageKernelLines("<6>[1.0] usb 1-1: new device\n<6>[1.1] eth0: link up\n<3>[2.0] sd 0:0:0:0: [sda] No Caching mode page found\n"),
                       ["[1.0] usb 1-1: new device", "[2.0] sd 0:0:0:0: [sda] No Caching mode page found"])
        XCTAssertEqual(OpenWrtModel.missingStoragePackages(["block-mount", "kmod-usb-storage"]).count, OpenWrtModel.usbStoragePackages.count - 2)
        XCTAssertTrue(OpenWrtModel.fstabFromDetect("config 'global'\n\toption\tanon_mount\t'0'").hasSuffix("'0'\n"))
        XCTAssertTrue(OpenWrtModel.fstabFromDetect("").hasPrefix("config global\n"))
        XCTAssertEqual(OpenWrtModel.outputTail("a\n\nb\nc\n", count: 2), ["b", "c"])

        var empty = OpenWrtStorageCheck()
        empty.packagesKnown = true
        empty.usbKnown = true
        empty.usbControllers = 1
        empty.partitionsKnown = true
        XCTAssertTrue(OpenWrtModel.storageFindings(empty)[0].contains("sees nothing plugged into its USB port"))
        var noHost = empty
        noHost.usbControllers = 0
        XCTAssertTrue(OpenWrtModel.storageFindings(noHost)[0].contains("no USB controller driver"))
        var noDriver = empty
        noDriver.usbDevices = ["Flash Disk (1-1)"]
        XCTAssertTrue(OpenWrtModel.storageFindings(noDriver)[0].contains("kmod-usb-storage"))
        var noBlock = noDriver
        noBlock.installed = ["kmod-usb-storage"]
        noBlock.partitions = ["sda", "sda1"]
        XCTAssertTrue(OpenWrtModel.storageFindings(noBlock)[0].contains("block-mount is not installed"))
        var noFstab = noBlock
        noFstab.blockTool = true
        noFstab.disks = 1
        let findings = OpenWrtModel.storageFindings(noFstab)
        XCTAssertTrue(findings.joined(separator: "\n").contains("/etc/config/fstab"))
        XCTAssertTrue(findings.last?.contains("Turn on Mount any disk") ?? false)
        var ready = noFstab
        ready.fstabExists = true
        ready.installed = OpenWrtModel.usbStoragePackages
        ready.mounted = 1
        XCTAssertEqual(OpenWrtModel.storageFindings(ready), ["The disk is mounted."])
        XCTAssertTrue(OpenWrtModel.storageReport(ready).hasPrefix("• The disk is mounted."))
        XCTAssertTrue(OpenWrtModel.storageReport(ready).contains("Storage packages: all installed"))
    }
}

final class OpenWrtClientTests: XCTestCase {
    private func client(_ state: FakeRouterState) -> (OpenWrtClient, FakeHTTPClient) {
        let http = FakeHTTPClient { request in try state.handle(request) }
        return (OpenWrtClient(http: http, base: URL(string: "http://192.168.1.1")), http)
    }

    func testSignInRenewalAndExec() async {
        let state = FakeRouterState()
        let (router, http) = client(state)
        let wrong = await router.signIn(user: "root", password: "guess")
        XCTAssertFalse(wrong.ok)
        XCTAssertTrue(wrong.error.contains("password"))
        let login = await router.signIn(user: "root", password: "secret")
        XCTAssertTrue(login.ok)
        state.expireOnce()
        let board = await router.call("system", "board")
        XCTAssertTrue(board.ok)
        XCTAssertEqual(board.data.str("model"), "Test Router")
        XCTAssertEqual(state.logins, 2)
        let renewals = await router.renewals
        XCTAssertEqual(renewals, 1)
        XCTAssertEqual(state.last("system", "board")?.first?.text, "sess2")
        let failed = await router.exec("/bin/false", [])
        XCTAssertFalse(failed.ok)
        XCTAssertEqual(failed.error, "nope")
        XCTAssertTrue(http.requests.allSatisfy { $0.url.path == "/ubus" && !$0.followRedirects })
    }

    func testUnreachable() async {
        let router = OpenWrtClient(http: FakeHTTPClient { _ in throw HTTPError.transport("connection refused") }, base: URL(string: "http://192.168.1.1"))
        let result = await router.signIn(user: "root", password: "secret")
        XCTAssertTrue(result.unreachable)
        XCTAssertTrue(result.error.contains("192.168.1.1"))
    }

    func testCertificateIsReported() async {
        let router = OpenWrtClient(http: FakeHTTPClient { _ in throw OpenWrtCertificateError(fingerprint: "AA:BB") }, base: URL(string: "https://192.168.1.1"))
        let result = await router.signIn(user: "root", password: "secret")
        XCTAssertEqual(result.certificate, "AA:BB")
    }

    func testRenewedSessionSeesNewlyInstalledShareConfig() async {
        let state = FakeRouterState()
        let (router, _) = client(state)
        _ = await router.signIn(user: "root", password: "secret")
        state.grantShareAclOnNextLogin()
        var stale = await router.loadSnapshot(now: 0)
        XCTAssertEqual(stale.shareConfig, "")
        XCTAssertFalse(OpenWrtModel.needsShareSessionRenewal(stale))
        stale.packages = [OpenWrtPackage(name: "ksmbd-server", version: "1"), OpenWrtPackage(name: "luci-app-ksmbd", version: "1")]
        XCTAssertTrue(OpenWrtModel.needsShareSessionRenewal(stale))
        stale.changes = ["network": [["set", "lan", "dns", "1.1.1.1"]]]
        XCTAssertFalse(OpenWrtModel.needsShareSessionRenewal(stale))

        let before = state.logins
        let renewed = await router.renewSession()
        XCTAssertTrue(renewed.ok)
        XCTAssertEqual(state.logins, before + 1)
        let fresh = await router.loadSnapshot(now: 0)
        XCTAssertEqual(fresh.shareConfig, "ksmbd")
        XCTAssertEqual(fresh.shares.first?.name, "Media")

        await router.signOut()
        let signedOut = state.logins
        let refused = await router.renewSession()
        XCTAssertFalse(refused.ok)
        XCTAssertEqual(state.logins, signedOut)
    }

    func testMissingShareConfigIsCreatedOnlyWhenReadable() async {
        let state = FakeRouterState()
        state.removeKsmbdConfig()
        let (router, _) = client(state)
        _ = await router.signIn(user: "root", password: "secret")
        state.setShareAcl(false)
        let denied = await router.createShareConfig()
        XCTAssertFalse(denied.ok)
        XCTAssertTrue(denied.error.contains("Sign out and sign in again"))
        XCTAssertNil(state.last("file", "write"))

        state.setShareAcl(true)
        let before = await router.loadSnapshot(now: 0)
        XCTAssertEqual(before.shareConfig, "")
        let created = await router.createShareConfig()
        XCTAssertTrue(created.ok)
        XCTAssertEqual(state.last("file", "write")?[3].str("path"), "/etc/config/ksmbd")
        XCTAssertEqual(state.last("file", "write")?[3].int("mode"), 0o600)
        let after = await router.loadSnapshot(now: 0)
        XCTAssertEqual(after.shareConfig, "ksmbd")
    }

    func testSnapshotStageAndApply() async {
        let state = FakeRouterState()
        let (router, _) = client(state)
        _ = await router.signIn(user: "root", password: "secret")
        let snapshot = await router.loadSnapshot(now: 0)
        XCTAssertEqual(snapshot.interfaceSections.map(\.name), ["lan", "wan"])
        XCTAssertEqual(snapshot.hostname, "OpenWrt")
        XCTAssertEqual(snapshot.packages.map(\.name), ["base-files", "luci"])
        XCTAssertEqual(snapshot.packageTool?.program, "/bin/opkg")
        XCTAssertEqual(snapshot.disks.map(\.block.name), ["sda1"])
        XCTAssertTrue(snapshot.autoMount)
        XCTAssertEqual(snapshot.shareConfig, "ksmbd")
        XCTAssertEqual(snapshot.shares.first?.name, "Media")

        let staged = await router.stage("network", "lan", UciEdit(values: ["dns": ["1.1.1.1"]], removed: ["gateway"]))
        XCTAssertTrue(staged.ok)
        XCTAssertEqual(state.last("uci", "set")?[3].str("section"), "lan")
        XCTAssertEqual(state.last("uci", "delete")?[3].strings("options"), ["gateway"])
        XCTAssertNil(state.last("uci", "apply"))

        state.failConfirm(times: 2)
        let waits = WaitLog()
        let outcome = await router.apply(rollback: true) { seconds in waits.add(seconds) }
        XCTAssertEqual(outcome, .confirmed)
        XCTAssertEqual(waits.values, [3, 2, 2])
        XCTAssertEqual(state.last("uci", "apply")?[3].bool("rollback"), true)

        state.failConfirm(times: 1000)
        let gone = await router.apply(rollback: true) { _ in }
        if case .rolledBack = gone {} else { XCTFail("an unreachable router rolls back, got \(gone)") }
        let plain = await router.apply(rollback: false) { _ in XCTFail("no waiting without rollback") }
        XCTAssertEqual(plain, .applied)

        let refused = await router.packageAction(OpenWrtModel.packageTools[3], action: "install", names: ["a;reboot"])
        XCTAssertFalse(refused.ok)
    }

    func testMissingFstabIsCreatedAndStorageChecked() async {
        let state = FakeRouterState()
        state.removeFstab()
        let (router, _) = client(state)
        _ = await router.signIn(user: "root", password: "secret")
        let snapshot = await router.loadSnapshot(now: 0)
        XCTAssertTrue(snapshot.fstabMissing)
        XCTAssertTrue(snapshot.problems.isEmpty)

        let created = await router.createFstab()
        XCTAssertTrue(created.ok)
        XCTAssertEqual(state.last("file", "write")?[3].str("path"), "/etc/config/fstab")
        XCTAssertTrue(state.last("file", "write")?[3].str("data").hasPrefix("config 'global'") ?? false)

        let (check, fresh) = await router.checkStorage(snapshot)
        XCTAssertFalse(fresh.fstabMissing)
        XCTAssertTrue(check.fstabExists)
        XCTAssertTrue(check.packagesKnown)
        XCTAssertEqual(check.usbControllers, 1)
        XCTAssertEqual(check.usbDevices, ["Flash Disk (1-1)"])
        XCTAssertEqual(check.partitions, ["sda", "sda1"])
        XCTAssertEqual(check.disks, 1)
        XCTAssertEqual(check.mounted, 1)
        XCTAssertEqual(check.kernelLines, ["[    5.1] usb 1-1: new high-speed USB device number 2"])
        XCTAssertTrue(OpenWrtModel.storageReport(check).contains("USB devices: Flash Disk (1-1)"))
    }
}

final class WaitLog: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [Double] = []
    var values: [Double] { lock.withLock { recorded } }
    func add(_ value: Double) { lock.withLock { recorded.append(value) } }
}
