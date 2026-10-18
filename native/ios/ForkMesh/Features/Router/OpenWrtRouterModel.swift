import FMBase
import FMFiles
import FMPackages
import FMVault
import Foundation
import Observation

struct OpenWrtVaultPrompt: Identifiable {
    enum Action {
        case save(title: String, url: String, username: String, password: String, done: String)
        case fill(url: String)
    }

    let id = UUID()
    let purpose: String
    let action: Action
}

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
    var storageLog: String = ""
    var storageBusy: Bool = false
    var routerHost: String = ""
    var saveToVault: Bool {
        didSet { UserDefaults.standard.set(saveToVault, forKey: Self.prefix + "saveLogin") }
    }
    var vaultPrompt: OpenWrtVaultPrompt?
    var vaultChoices: [VaultItem] = []

    @ObservationIgnored private var client: OpenWrtClient?
    @ObservationIgnored private var loginUrl = ""
    @ObservationIgnored private var loginUser = ""
    @ObservationIgnored private var filledFromVault = ""
    @ObservationIgnored private var http: OpenWrtHTTPClient?
    @ObservationIgnored private var certificateKey: String = ""

    init() {
        let defaults = UserDefaults.standard
        address = defaults.string(forKey: Self.prefix + "address") ?? ""
        user = defaults.string(forKey: Self.prefix + "user") ?? "root"
        saveToVault = defaults.object(forKey: Self.prefix + "saveLogin") as? Bool ?? true
    }

    var vault: VaultService? { Services.get(VaultService.self) }

    var vaultOffered: Bool {
        guard let vault else { return false }
        return vault.phase != .signedOut
    }

    private static func origin(_ base: URL) -> String {
        guard var parts = URLComponents(url: base, resolvingAgainstBaseURL: false) else { return "" }
        parts.user = nil
        parts.password = nil
        parts.path = ""
        parts.query = nil
        parts.fragment = nil
        return parts.string ?? ""
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
        let secret = password
        let result = await router.signIn(user: account, password: secret)
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
        loginUrl = Self.origin(base)
        loginUser = account
        let fromVault = !filledFromVault.isEmpty && secret == filledFromVault
        filledFromVault = ""
        let connected = "Connected to \(host)."
        show(connected)
        await refresh()
        if !fromVault && saveToVault && vaultOffered { await saveInVault(secret, done: connected) }
    }

    func saveInVault(_ secret: String, done: String) async {
        guard !loginUrl.isEmpty, !secret.isEmpty else { return }
        let host = URLComponents(string: loginUrl)?.host ?? routerHost
        await perform(.save(title: "OpenWrt router (\(host))", url: loginUrl, username: loginUser.isEmpty ? "root" : loginUser,
                            password: secret, done: done),
                      purpose: "Router asks to save the password for \(loginUrl) in your Password Vault.")
    }

    func signInWithVault() async {
        let typed = address.trimmingCharacters(in: .whitespaces).isEmpty ? "192.168.1.1" : address
        let parsed = OpenWrtModel.parseAddress(typed)
        guard parsed.valid, let base = parsed.base else {
            show(parsed.error, error: true)
            return
        }
        let url = Self.origin(base)
        await perform(.fill(url: url), purpose: "Router asks for the password you saved for \(url).")
    }

    func vaultUnlocked() async {
        guard let prompt = vaultPrompt else { return }
        vaultPrompt = nil
        await perform(prompt.action, purpose: prompt.purpose)
    }

    func vaultPromptCancelled() {
        guard let prompt = vaultPrompt else { return }
        vaultPrompt = nil
        if case let .save(_, _, _, _, done) = prompt.action {
            show("\(done) The password was not saved in Password Vault.")
        } else {
            show("")
        }
    }

    func useVaultLogin(_ item: VaultItem) async {
        vaultChoices = []
        user = item.username.isEmpty ? "root" : item.username
        password = item.password
        filledFromVault = item.password
        await connect()
    }

    private func perform(_ action: OpenWrtVaultPrompt.Action, purpose: String) async {
        guard let vault else { return }
        switch action {
        case let .save(title, url, username, secret, done):
            switch await vault.saveLogin(title: title, url: url, username: username, password: secret) {
            case let .saved(changed):
                show(changed ? "\(done) The password is saved in Password Vault." : "\(done) The password was already saved in Password Vault.")
            case .locked:
                vaultPrompt = OpenWrtVaultPrompt(purpose: purpose, action: action)
            case .noVault:
                show("\(done) The password was not saved: create a Password Vault first in Tools, Vault.", error: true)
            case .signedOut:
                show("\(done) Sign in to ForkMesh to save the password in Password Vault.", error: true)
            case let .failed(text):
                show("\(done) The password was not saved in Password Vault: \(text)", error: true)
            case .found:
                break
            }
        case let .fill(url):
            switch await vault.findLogins(url: url) {
            case let .found(items):
                await choose(items, url: url)
            case .locked:
                vaultPrompt = OpenWrtVaultPrompt(purpose: purpose, action: action)
            case .noVault:
                show("There is no Password Vault on this account yet. Create one in Tools, Vault.", error: true)
            case .signedOut:
                show("Sign in to ForkMesh to use Password Vault.", error: true)
            case let .failed(text):
                show(text, error: true)
            case .saved:
                break
            }
        }
    }

    private func choose(_ items: [VaultItem], url: String) async {
        if items.isEmpty {
            show("Password Vault has no password for \(url) yet. Type it once with “Save the password in Password Vault” on.", error: true)
            return
        }
        let typed = user.trimmingCharacters(in: .whitespaces).isEmpty ? "root" : user.trimmingCharacters(in: .whitespaces)
        if let same = items.first(where: { $0.username == typed }) {
            await useVaultLogin(same)
        } else if items.count == 1 {
            await useVaultLogin(items[0])
        } else {
            vaultChoices = items
        }
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
        storageLog = ""
        storageBusy = false
        routerHost = ""
        loginUrl = ""
        loginUser = ""
        FilesLibrary.shared.routerShares = []
    }

    private func dropSession(_ text: String) async {
        http?.invalidate()
        client = nil
        http = nil
        signedIn = false
        snapshot = OpenWrtSnapshot()
        storageLog = ""
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
        FilesLibrary.shared.routerShares = fresh.shares.map { RouterShareSuggestion(name: $0.name, host: routerHost, guests: $0.guests) }
    }

    func reloadConfig(_ config: String) async {
        guard let client else { return }
        let result = await client.call("uci", "get", ["config": .string(config)])
        if result.ok {
            snapshot.configs[config] = result.data.obj("values")
            snapshot.missingConfigs.remove(config)
        }
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
        guard let client else { return }
        if snapshot.fstabMissing {
            guard on, !storageBusy else { return }
            storageBusy = true
            show("The router has no storage settings file (/etc/config/fstab) yet. Creating it…")
            let created = await client.createFstab()
            storageBusy = false
            guard created.ok else {
                show(created.error, error: true)
                await checkStorage()
                return
            }
            appendStorageLog("Created /etc/config/fstab on the router.")
            let fstab = await client.call("uci", "get", ["config": "fstab"])
            guard fstab.ok else { return show("ForkMesh created /etc/config/fstab, but the router cannot read it: \(fstab.error)", error: true) }
            snapshot.configs["fstab"] = fstab.data.obj("values")
            snapshot.missingConfigs.remove("fstab")
        }
        let values: JSONObject = ["anon_mount": .string(on ? "1" : "0"), "auto_mount": "1"]
        let saved: Bool
        if let global = snapshot.sections("fstab", type: "global").first {
            saved = await stage("fstab", global.name, UciEdit(values: values), what: "Automatic mounting")
        } else {
            saved = await add("fstab", type: "global", values: values, what: "Automatic mounting")
        }
        if saved {
            show(on ? "Automatic mounting saved. Apply the change, then press Mount disks now." : "Automatic mounting turned off. Apply to make it take effect.")
        } else {
            show("Could not change automatic mounting: \(message)", error: true)
            await checkStorage()
        }
    }

    func appendStorageLog(_ line: String) {
        storageLog = storageLog.isEmpty ? line : storageLog + "\n" + line
    }

    func checkStorage(keepLog: Bool = false) async {
        guard let client, !storageBusy else { return }
        storageBusy = true
        defer { storageBusy = false }
        if keepLog { appendStorageLog("\nChecking USB storage…") } else { storageLog = "Checking USB storage…" }
        let (check, fresh) = await client.checkStorage(snapshot)
        snapshot = fresh
        let report = OpenWrtModel.storageReport(check)
        if keepLog { appendStorageLog(report) } else { storageLog = report }
    }

    func installStorageSupport() async {
        guard !storageBusy else { return }
        guard let failed = await installPackages(
            OpenWrtModel.usbStoragePackages, progress: "Installing USB storage support…",
            noTool: "The router does not let ForkMesh run its package manager, so USB storage support cannot be installed from here. Install luci-app-package-manager (or luci-app-opkg on older releases) on the router.")
        else {
            await checkStorage()
            return
        }
        if failed.isEmpty {
            show("USB storage support installed.")
        } else {
            show("Some storage packages could not be installed: \(failed.map { $0.name }.joined(separator: ", ")). The log below says why.", error: true)
        }
        await checkStorage(keepLog: true)
    }

    func installShareSupport() async {
        guard !storageBusy else { return }
        guard let failed = await installPackages(
            OpenWrtModel.shareSupportPackages, progress: "Installing ksmbd-server and luci-app-ksmbd…",
            noTool: "The router does not let ForkMesh run its package manager, so network sharing cannot be installed from here. Install luci-app-package-manager (or luci-app-opkg on older releases) on the router.")
        else { return }
        if let first = failed.first {
            show("Could not install \(first.name): \(first.tail)", error: true)
        } else if snapshot.shareConfig.isEmpty {
            show("The packages are installed, but the router shows no file server settings yet. Pull to refresh in a moment.", error: true)
        } else {
            show("Network sharing is installed. Share a mounted disk below.")
        }
    }

    private func installPackages(_ names: [String], progress: String, noTool: String) async -> [(name: String, tail: String)]? {
        guard let client else { return nil }
        guard let tool = snapshot.packageTool else {
            show(noTool, error: true)
            return nil
        }
        storageBusy = true
        show(progress)
        storageLog = "Updating the router's package lists…"
        let update = await client.packageAction(tool, action: "update", names: [])
        if update.ok {
            appendStorageLog("Package lists updated.")
        } else {
            appendStorageLog("Updating the package lists failed; the router may have no internet connection. Trying to install anyway.")
            OpenWrtModel.outputTail(update.error).forEach { appendStorageLog("  " + $0) }
        }
        var failed: [(name: String, tail: String)] = []
        for name in names {
            appendStorageLog("Installing \(name)…")
            let result = await client.packageAction(tool, action: "install", names: [name])
            if result.ok {
                appendStorageLog("  \(name) is installed.")
            } else {
                appendStorageLog("  \(name) failed:")
                let output = (result.data.str("stdout") + "\n" + result.data.str("stderr")).trimmingCharacters(in: .whitespacesAndNewlines)
                let tail = OpenWrtModel.outputTail(output.isEmpty ? result.error : output)
                tail.forEach { appendStorageLog("    " + $0) }
                failed.append((name, tail.isEmpty ? result.error : tail.joined(separator: " ")))
            }
        }
        storageBusy = false
        await refresh()
        return failed
    }

    func applyAndMount() async {
        await apply()
        guard snapshot.pendingCount == 0 else { return }
        await mountAll()
        await checkStorage()
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
        guard !config.isEmpty else { return show("Install ksmbd-server and luci-app-ksmbd to share disks: press Install network sharing under Network shares.", error: true) }
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
        if result.ok && saveToVault && vaultOffered { await saveInVault(password, done: "Router password changed.") }
        return result.ok
    }
}
