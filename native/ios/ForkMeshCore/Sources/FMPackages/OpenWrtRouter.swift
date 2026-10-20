import FMBase
import Foundation

public struct OpenWrtSnapshot: Equatable, Sendable {
    public var board: JSONObject = JSONObject()
    public var info: JSONObject = JSONObject()
    public var interfaces: [OpenWrtInterfaceStatus] = []
    public var wirelessStatus: JSONObject = JSONObject()
    public var configs: [String: JSONObject] = [:]
    public var configNames: [String] = []
    public var missingConfigs: Set<String> = []
    public var changes: JSONObject = JSONObject()
    public var leases: [OpenWrtLease] = []
    public var services: [OpenWrtService] = []
    public var packages: [OpenWrtPackage] = []
    public var packageTool: OpenWrtPackageTool?
    public var blocks: [OpenWrtBlockDevice] = []
    public var mounts: [OpenWrtMountPoint] = []
    public var storageProblem: String = ""
    public var problems: [String] = []

    public init() {}

    public func sections(_ config: String, type: String = "") -> [UciSection] {
        OpenWrtModel.uciSections(configs[config] ?? JSONObject(), type: type)
    }

    public func section(_ config: String, _ name: String) -> UciSection? {
        name.isEmpty ? nil : sections(config).first { $0.name == name }
    }

    public var hostname: String {
        let configured = sections("system", type: "system").first?.option("hostname") ?? ""
        return configured.isEmpty ? board.str("hostname") : configured
    }

    public var pendingCount: Int { OpenWrtModel.pendingCount(changes) }
    public var pendingLines: [String] { OpenWrtModel.describeChanges(changes) }
    public var interfaceSections: [UciSection] { sections("network", type: "interface").filter { $0.name != "loopback" } }
    public var radios: [UciSection] { sections("wireless", type: "wifi-device") }
    public func wifiNetworks(radio: String) -> [UciSection] {
        sections("wireless", type: "wifi-iface").filter { $0.option("device") == radio }
    }
    public var reservations: [UciSection] { sections("dhcp", type: "host") }
    public var forwards: [UciSection] {
        sections("firewall", type: "redirect").filter { $0.option("target", "DNAT") == "DNAT" }
    }
    public var disks: [OpenWrtDisk] { OpenWrtModel.storageDisks(blocks, mounts: mounts, fstab: sections("fstab")) }
    public var shareConfig: String { OpenWrtModel.shareConfig(configNames) }
    public var shares: [OpenWrtShare] { shareConfig.isEmpty ? [] : OpenWrtModel.parseShares(sections(shareConfig)) }
    public var autoMount: Bool { sections("fstab", type: "global").first?.flag("anon_mount") ?? false }
    public var fstabMissing: Bool { missingConfigs.contains("fstab") }

    public func dhcpSection(for interface: String) -> UciSection? {
        sections("dhcp", type: "dhcp").first { $0.option("interface") == interface }
    }

    public func status(of interface: String) -> OpenWrtInterfaceStatus? {
        interfaces.first { $0.name == interface }
    }

    public func reservation(for mac: String) -> UciSection? {
        reservations.first { $0.option("mac").lowercased().split(separator: " ").contains(Substring(mac.lowercased())) }
    }
}

public enum OpenWrtApplyOutcome: Equatable, Sendable {
    case nothing
    case applied
    case confirmed
    case rolledBack(String)
    case failed(String)

    public var message: String {
        switch self {
        case .nothing: return "There was nothing to apply."
        case .applied: return "Applied. The router is reloading the changed services."
        case .confirmed: return "Applied and confirmed. The router kept the new settings."
        case .rolledBack(let message), .failed(let message): return message
        }
    }

    public var isError: Bool {
        switch self {
        case .rolledBack, .failed: return true
        default: return false
        }
    }
}

public typealias OpenWrtWait = @Sendable (Double) async -> Void

extension OpenWrtClient {
    public static let baseConfigs = ["network", "wireless", "dhcp", "firewall", "system", "fstab"]

    public func loadSnapshot(now: Int64 = Int64(Date().timeIntervalSince1970), packageTool: OpenWrtPackageTool? = nil) async -> OpenWrtSnapshot {
        var snapshot = OpenWrtSnapshot()
        let board = await call("system", "board")
        if board.ok { snapshot.board = board.data } else { snapshot.problems.append(board.error) }
        snapshot.info = await call("system", "info").data
        snapshot.interfaces = OpenWrtModel.parseInterfaces(await call("network.interface", "dump").data)
        snapshot.wirelessStatus = await call("network.wireless", "status").data
        let configs = await call("uci", "configs")
        snapshot.configNames = configs.data.strings("configs").sorted()
        var wanted = Self.baseConfigs
        let shares = OpenWrtModel.shareConfig(snapshot.configNames)
        if !shares.isEmpty { wanted.append(shares) }
        for config in wanted {
            let result = await call("uci", "get", ["config": .string(config)])
            if result.ok || result.status == UbusStatus.notFound.rawValue {
                snapshot.configs[config] = result.data.obj("values")
                if !result.ok { snapshot.missingConfigs.insert(config) }
            } else {
                snapshot.problems.append("Could not read the \(config) settings: \(result.error)")
            }
        }
        snapshot.changes = await call("uci", "changes").data.obj("changes")
        let leases = await call("luci-rpc", "getDHCPLeases")
        if leases.ok {
            snapshot.leases = OpenWrtModel.parseLuciLeases(leases.data, now: now)
        } else {
            let file = await call("file", "read", ["path": "/tmp/dhcp.leases"])
            snapshot.leases = file.ok ? OpenWrtModel.parseLeases(file.data.str("data")) : []
        }
        snapshot.services = OpenWrtModel.parseServices(await call("rc", "list").data)
        let blocks = await call("luci", "getBlockDevices")
        snapshot.blocks = blocks.ok ? OpenWrtModel.parseBlockDevices(blocks.data) : []
        snapshot.storageProblem = blocks.ok ? "" : blocks.error
        snapshot.mounts = OpenWrtModel.parseMountPoints(await call("luci", "getMountPoints").data)
        let packages = await listPackages(preferred: packageTool)
        snapshot.packageTool = packages.tool
        snapshot.packages = packages.packages
        if snapshot.shareConfig.isEmpty {
            for config in OpenWrtModel.shareConfigCandidates(packages.packages.map(\.name)) {
                let result = await call("uci", "get", ["config": .string(config)])
                guard result.ok else { continue }
                snapshot.configs[config] = result.data.obj("values")
                snapshot.configNames = (snapshot.configNames + [config]).sorted()
                break
            }
        }
        return snapshot
    }

    public func listPackages(preferred: OpenWrtPackageTool?) async -> (tool: OpenWrtPackageTool?, packages: [OpenWrtPackage]) {
        let tools = preferred.map { [$0] } ?? OpenWrtModel.packageTools
        for tool in tools {
            let result = await exec(tool.program, OpenWrtModel.packageArguments(tool, action: "list"))
            let output = result.data.str("stdout")
            if result.ok && !output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                return (tool, OpenWrtModel.parseInstalledPackages(output))
            }
        }
        return (nil, [])
    }

    public func stage(_ config: String, _ section: String, _ edit: UciEdit) async -> OpenWrtResult {
        var result = OpenWrtResult(ok: true, status: 0)
        if !edit.values.isEmpty {
            result = await call("uci", "set", ["config": .string(config), "section": .string(section), "values": .object(edit.values)])
            guard result.ok else { return result }
        }
        if !edit.removed.isEmpty {
            result = await call("uci", "delete", ["config": .string(config), "section": .string(section), "options": JSONValue(edit.removed)])
        }
        return result
    }

    public func addSection(_ config: String, type: String, name: String = "", values: JSONObject) async -> OpenWrtResult {
        var args: JSONObject = ["config": .string(config), "type": .string(type), "values": .object(values)]
        if !name.isEmpty { args["name"] = .string(name) }
        return await call("uci", "add", args)
    }

    public func deleteSection(_ config: String, _ section: String) async -> OpenWrtResult {
        await call("uci", "delete", ["config": .string(config), "section": .string(section)])
    }

    public func discard(_ changes: JSONObject) async -> OpenWrtResult {
        var last = OpenWrtResult(ok: true, status: 0)
        for config in changes.keys {
            let result = await call("uci", "revert", ["config": .string(config)])
            if !result.ok { last = result }
        }
        return last
    }

    public func apply(rollback: Bool, wait: OpenWrtWait) async -> OpenWrtApplyOutcome {
        let result = await call("uci", "apply", ["rollback": .bool(rollback), "timeout": .int(Int64(OpenWrt.rollbackSeconds))])
        if !result.ok {
            return result.status == UbusStatus.noData.rawValue ? .nothing : .failed(result.error)
        }
        if !rollback { return .applied }
        let budget = Double(OpenWrt.rollbackSeconds - 10)
        var waited = 3.0
        await wait(3)
        while true {
            let confirm = await call("uci", "confirm", timeout: 4)
            if confirm.ok { return .confirmed }
            if (confirm.unreachable || confirm.accessDenied) && waited < budget {
                await wait(2)
                waited += 2
                continue
            }
            if confirm.unreachable {
                return .rolledBack("The router stopped answering after the change, so it will put the old settings back by itself within \(OpenWrt.rollbackSeconds) seconds.")
            }
            return .failed("The change could not be confirmed (\(confirm.error)). The router will put the old settings back by itself.")
        }
    }

    public func createFstab() async -> OpenWrtResult {
        let detect = await exec("/sbin/block", ["detect"], timeout: 60)
        guard detect.ok else {
            return .failure("Automatic mounting needs block-mount, and the router could not run it (\(detect.error)). Press Install USB storage support first.")
        }
        let written = await call("file", "write", ["path": .string(OpenWrtModel.fstabPath), "data": .string(OpenWrtModel.fstabFromDetect(detect.data.str("stdout"))), "mode": .int(0o644)])
        guard written.ok else {
            return .failure("The router has no /etc/config/fstab and did not let ForkMesh create it (\(written.error)). Reinstall block-mount, which creates it, or run \"block detect > /etc/config/fstab\" on the router.")
        }
        return written
    }

    public func createShareConfig() async -> OpenWrtResult {
        let existing = await call("uci", "get", ["config": "ksmbd"])
        if existing.ok { return existing }
        guard existing.status == UbusStatus.notFound.rawValue else {
            return .failure("The packages are installed, but the router did not let ForkMesh read the file server settings (\(existing.error)). Sign out and sign in again.")
        }
        let written = await call("file", "write", ["path": .string(OpenWrtModel.ksmbdPath), "data": .string(OpenWrtModel.defaultKsmbd), "mode": .int(0o600)])
        guard written.ok else {
            return .failure("The packages are installed, but the router has no file server settings (/etc/config/ksmbd) and did not let ForkMesh create them (\(written.error)). Remove and reinstall ksmbd-server on the router, which creates the file.")
        }
        let loaded = await call("uci", "get", ["config": "ksmbd"])
        guard loaded.ok else { return .failure("ForkMesh created /etc/config/ksmbd, but the router cannot read it: \(loaded.error)") }
        return loaded
    }

    public func checkStorage(_ current: OpenWrtSnapshot) async -> (check: OpenWrtStorageCheck, snapshot: OpenWrtSnapshot) {
        var snapshot = current
        var check = OpenWrtStorageCheck()
        if let tool = snapshot.packageTool {
            let listed = await listPackages(preferred: tool)
            if listed.tool != nil { snapshot.packages = listed.packages }
            check.packagesKnown = true
        }
        check.installed = snapshot.packages.map(\.name)
        check.blockTool = await call("file", "stat", ["path": "/sbin/block"]).ok
        let fstab = await call("uci", "get", ["config": "fstab"])
        check.fstabExists = fstab.ok
        if fstab.ok || fstab.status == UbusStatus.notFound.rawValue {
            snapshot.configs["fstab"] = fstab.data.obj("values")
            if fstab.ok { snapshot.missingConfigs.remove("fstab") } else { snapshot.missingConfigs.insert("fstab") }
        }
        let usb = await call("file", "list", ["path": "/sys/bus/usb/devices"])
        check.usbKnown = usb.ok
        let entries = usb.data.maps("entries").map { $0.str("name") }
        check.usbControllers = OpenWrtModel.usbControllerCount(entries)
        for entry in OpenWrtModel.usbDeviceEntries(entries).prefix(8) {
            let product = await call("file", "read", ["path": .string("/sys/bus/usb/devices/\(entry)/product")])
            let name = product.ok ? product.data.str("data").trimmingCharacters(in: .whitespacesAndNewlines) : ""
            check.usbDevices.append(name.isEmpty ? entry : "\(name) (\(entry))")
        }
        let partitions = await call("file", "read", ["path": "/proc/partitions"])
        check.partitionsKnown = partitions.ok
        check.partitions = OpenWrtModel.parsePartitions(partitions.data.str("data"))
        let blocks = await call("luci", "getBlockDevices")
        snapshot.blocks = blocks.ok ? OpenWrtModel.parseBlockDevices(blocks.data) : []
        snapshot.storageProblem = blocks.ok ? "" : blocks.error
        snapshot.mounts = OpenWrtModel.parseMountPoints(await call("luci", "getMountPoints").data)
        snapshot.changes = await call("uci", "changes").data.obj("changes")
        check.kernelLines = OpenWrtModel.storageKernelLines(await exec("/bin/dmesg", ["-r"]).data.str("stdout"))
        let disks = snapshot.disks
        check.disks = disks.count
        check.mounted = disks.filter(\.mounted).count
        check.listProblem = snapshot.storageProblem
        check.autoMount = snapshot.autoMount
        check.autoMountPending = snapshot.changes.contains("fstab")
        return (check, snapshot)
    }

    public func packageAction(_ tool: OpenWrtPackageTool, action: String, names: [String]) async -> OpenWrtResult {
        if action != "update" && (names.isEmpty || !names.allSatisfy(OpenWrtModel.validPackageName)) {
            return .failure(names.isEmpty ? "Choose a package first." : "That is not a package name.")
        }
        let result = await exec(tool.program, OpenWrtModel.packageArguments(tool, action: action, names: names), timeout: 300)
        guard action == "install", !result.ok, result.timedOut else { return result }
        let deadline = Date().addingTimeInterval(180)
        while true {
            if OpenWrtModel.packagesInstalled(await listPackages(preferred: tool).packages, names) {
                return OpenWrtResult(ok: true, status: 0)
            }
            if Date() >= deadline { break }
            try? await Task.sleep(nanoseconds: 5_000_000_000)
        }
        var failed = result
        failed.error = "The router stopped answering during the install, and \(names.joined(separator: ", ")) still is not installed after three minutes. The router may have no internet connection, or the download is still running: check Packages later, or try again."
        return failed
    }
}
