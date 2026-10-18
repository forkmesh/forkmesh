import FMBase
import FMPackages
import Foundation
import Observation

@MainActor
@Observable
final class OpenWrtRouterModel {
    static let criticalServices: Set<String> = ["network", "firewall", "dropbear", "uhttpd", "rpcd", "dnsmasq", "odhcpd", "ubus", "log"]
    private static let prefix = "market/packages/openwrt/data/"

    var address: String
    var user: String
    var password: String = ""
    var signedIn: Bool = false
    var busy: Bool = false
    var message: String = ""
    var messageIsError: Bool = false
    var pendingCertificate: String = ""
    var rollback: Bool = true
    var snapshot = OpenWrtSnapshot()
    var packageOutput: String = ""
    var routerHost: String = ""

    @ObservationIgnored private var client: OpenWrtClient?
    @ObservationIgnored private var http: OpenWrtHTTPClient?
    @ObservationIgnored private var certificateKey: String = ""

    init() {
        let defaults = UserDefaults.standard
        address = defaults.string(forKey: Self.prefix + "address") ?? ""
        user = defaults.string(forKey: Self.prefix + "user") ?? "root"
    }

    var addressNote: String {
        let parsed = OpenWrtModel.parseAddress(address.isEmpty ? "192.168.1.1" : address)
        if !parsed.valid { return parsed.error }
        if parsed.encrypted { return "HTTPS: the first time, you are asked to check the router's certificate fingerprint." }
        return "Plain HTTP: the password crosses your Wi-Fi unencrypted. Install luci-ssl on the router and use https:// to encrypt it."
    }

    func show(_ text: String, error: Bool = false) {
        message = text
        messageIsError = error
    }

    func connect() async {
        let typed = address.trimmingCharacters(in: .whitespaces).isEmpty ? "192.168.1.1" : address
        let parsed = OpenWrtModel.parseAddress(typed)
        guard parsed.valid, let base = parsed.base, var components = URLComponents(url: base, resolvingAgainstBaseURL: false) else {
            show(parsed.error, error: true)
            return
        }
        busy = true
        defer { busy = false }
        let host = parsed.host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        show("Connecting to \(host)…")
        if !OpenWrtModel.isLiteralAddress(host) {
            let addresses = await Task.detached { OpenWrtHTTPClient.resolve(host) }.value
            guard !addresses.isEmpty else {
                show("\(host) could not be found on your network.", error: true)
                return
            }
            guard addresses.allSatisfy(OpenWrtModel.isLocalHost) else {
                show("\(host) is not on your local network. Only routers on your own network can be managed, so the router password never crosses the internet.", error: true)
                return
            }
            let chosen = addresses.first { OpenWrtModel.ipv4Bytes($0) != nil } ?? addresses[0]
            components.host = chosen.contains(":") ? "[\(chosen)]" : chosen
        }
        guard let target = components.url else {
            show("That address cannot be used.", error: true)
            return
        }
        certificateKey = Self.prefix + "certificates/" + "\(host):\(components.port ?? (parsed.encrypted ? 443 : 80))"
        http?.invalidate()
        let transport = OpenWrtHTTPClient(trusted: UserDefaults.standard.string(forKey: certificateKey) ?? "")
        http = transport
        let router = OpenWrtClient(http: transport, base: target)
        let account = user.trimmingCharacters(in: .whitespaces).isEmpty ? "root" : user.trimmingCharacters(in: .whitespaces)
        let result = await router.signIn(user: account, password: password)
        guard result.ok else {
            pendingCertificate = result.certificate
            show(result.error, error: true)
            return
        }
        client = router
        pendingCertificate = ""
        password = ""
        routerHost = host
        UserDefaults.standard.set(address, forKey: Self.prefix + "address")
        UserDefaults.standard.set(account, forKey: Self.prefix + "user")
        signedIn = true
        show("Connected to \(host).")
        await refresh()
    }

    func trustCertificate() async {
        guard !pendingCertificate.isEmpty, !certificateKey.isEmpty else { return }
        UserDefaults.standard.set(pendingCertificate, forKey: certificateKey)
        await connect()
    }

    func signOut() async {
        await client?.signOut()
        http?.invalidate()
        client = nil
        http = nil
        signedIn = false
        snapshot = OpenWrtSnapshot()
        packageOutput = ""
        routerHost = ""
    }

    private func dropSession(_ text: String) async {
        http?.invalidate()
        client = nil
        http = nil
        signedIn = false
        snapshot = OpenWrtSnapshot()
        show(text)
    }

    func refresh() async {
        guard let client else { return }
        busy = true
        defer { busy = false }
        let before = await client.renewals
        let fresh = await client.loadSnapshot(packageTool: snapshot.packageTool)
        let after = await client.renewals
        if after > before && snapshot.pendingCount > 0 {
            show("The router session timed out, so the router dropped the changes you had not applied. ForkMesh signed in again.", error: true)
        } else if let problem = fresh.problems.first {
            show(problem, error: true)
        }
        snapshot = fresh
    }

    func reloadConfig(_ config: String) async {
        guard let client else { return }
        let result = await client.call("uci", "get", ["config": .string(config)])
        if result.ok { snapshot.configs[config] = result.data.obj("values") }
        snapshot.changes = await client.call("uci", "changes").data.obj("changes")
    }

    @discardableResult
    func stage(_ config: String, _ section: String, _ edit: UciEdit, what: String) async -> Bool {
        guard let client else { return false }
        let result = await client.stage(config, section, edit)
        await reloadConfig(config)
        report(result, "\(what) saved. Apply to make it take effect.")
        return result.ok
    }

    @discardableResult
    func add(_ config: String, type: String, name: String = "", values: JSONObject, what: String) async -> Bool {
        guard let client else { return false }
        let result = await client.addSection(config, type: type, name: name, values: values)
        await reloadConfig(config)
        report(result, "\(what) added. Apply to make it take effect.")
        return result.ok
    }

    func delete(_ config: String, _ section: String, what: String) async {
        guard let client else { return }
        let result = await client.deleteSection(config, section)
        await reloadConfig(config)
        report(result, "\(what) deleted. Apply to make it take effect.")
    }

    func apply() async {
        guard let client else { return }
        busy = true
        show("Applying changes…")
        let outcome = await client.apply(rollback: rollback) { seconds in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
        }
        busy = false
        show(outcome.message, error: outcome.isError)
        if outcome == .applied { try? await Task.sleep(nanoseconds: 3_000_000_000) }
        await refresh()
    }

    func discard() async {
        guard let client else { return }
        let result = await client.discard(snapshot.changes)
        report(result, "Changes discarded.")
        await refresh()
    }

    func report(_ result: OpenWrtResult, _ success: String) {
        if result.ok { show(success) } else { show(result.error, error: true) }
    }

    func restartInterface(_ name: String) async {
        guard let client else { return }
        let down = await client.call("network.interface", "down", ["interface": .string(name)])
        guard down.ok else { return show(down.error, error: true) }
        let up = await client.call("network.interface", "up", ["interface": .string(name)])
        report(up, "Interface \(name) restarted.")
        try? await Task.sleep(nanoseconds: 3_000_000_000)
        await refresh()
    }

    func serviceAction(_ name: String, _ action: String) async {
        guard let client else { return }
        let result = await client.call("rc", "init", ["name": .string(name), "action": .string(action)], timeout: 60)
        report(result, "\(name): \(action) done.")
        snapshot.services = OpenWrtModel.parseServices(await client.call("rc", "list").data)
    }

    func packageAction(_ action: String, _ names: [String]) async {
        guard let client else { return }
        guard let tool = snapshot.packageTool else {
            return show("The router does not let ForkMesh run its package manager. Install luci-app-package-manager (or luci-app-opkg) on the router.", error: true)
        }
        busy = true
        packageOutput = "Running \(([tool.program] + OpenWrtModel.packageArguments(tool, action: action, names: names)).joined(separator: " "))…"
        let result = await client.packageAction(tool, action: action, names: names)
        busy = false
        let output = [result.data.str("stdout"), result.data.str("stderr"), result.ok ? "" : result.error]
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        packageOutput = output.isEmpty ? "Done." : output.joined(separator: "\n")
        let subject = names.joined(separator: ", ")
        report(result, action == "update" ? "Package lists updated." : action == "install" ? "\(subject) installed." : "\(subject) removed.")
        if action != "update" {
            let listed = await client.listPackages(preferred: tool)
            snapshot.packages = listed.packages
        }
        if action == "install" && result.ok { await refresh() }
    }

    func unmount(_ disk: OpenWrtDisk) async {
        guard let client else { return }
        let result = await client.exec("/bin/umount", [disk.mount], timeout: 60)
        report(result, "\(disk.mount) unmounted. You can unplug the disk now.")
        await refresh()
    }

    func mountAll() async {
        guard let client else { return }
        if snapshot.changes.contains("fstab") {
            show("Apply your storage changes first: the router mounts what is saved, not what is waiting.", error: true)
        }
        let result = await client.exec("/sbin/block", ["mount"], timeout: 60)
        report(result, "The router mounted its disks.")
        await refresh()
    }

    func setAutoMount(_ on: Bool) async {
        let values: JSONObject = ["anon_mount": .string(on ? "1" : "0"), "auto_mount": "1"]
        if let global = snapshot.sections("fstab", type: "global").first {
            await stage("fstab", global.name, UciEdit(values: values), what: "Automatic mounting")
        } else {
            await add("fstab", type: "global", values: values, what: "Automatic mounting")
        }
    }

    func mountAtBoot(_ disk: OpenWrtDisk, path: String) async {
        if let problem = OpenWrtModel.mountPathProblem(path) { return show(problem, error: true) }
        if disk.fstabSection.isEmpty {
            await add("fstab", type: "mount", values: OpenWrtModel.mountSectionValues(disk.block, target: path), what: "Mount for \(disk.title)")
        } else {
            await stage("fstab", disk.fstabSection, UciEdit(values: ["target": .string(path), "enabled": "1"]), what: "Mount for \(disk.title)")
        }
    }

    func share(_ disk: OpenWrtDisk, name: String, guests: Bool) async {
        let config = snapshot.shareConfig
        guard !config.isEmpty else { return show("Install ksmbd-server and luci-app-ksmbd on the Packages screen to share disks.", error: true) }
        if let problem = OpenWrtModel.shareNameProblem(name) { return show(problem, error: true) }
        await add(config, type: OpenWrtModel.shareSectionType(config), values: OpenWrtModel.shareValues(name: name, path: disk.mount, guests: guests), what: "Share \(name)")
    }

    func reboot() async {
        guard let client else { return }
        let result = await client.call("system", "reboot")
        if !result.ok && !result.unreachable { return show(result.error, error: true) }
        await dropSession("The router is restarting. Connect again in a minute or two.")
    }

    func changePassword(_ password: String, confirm: String) async -> Bool {
        guard let client else { return false }
        if let problem = OpenWrtModel.passwordProblem(password, confirm: confirm) {
            show(problem, error: true)
            return false
        }
        let account = await client.user
        let result = await client.call("luci", "setPassword", ["username": .string(account.isEmpty ? "root" : account), "password": .string(password)])
        if result.ok { await client.rememberPassword(password) }
        report(result, "Router password changed.")
        return result.ok
    }
}
