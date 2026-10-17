import Foundation

public enum PhoneSecurityScanner {
    public static func scan(_ facts: PhoneSecurityFacts) -> [PhoneSecurityFinding] {
        let findings: [PhoneSecurityFinding] = [
            passcode(facts), biometrics(facts), notificationPreviews(facts),
            paths(facts, PhoneSecurityCatalog.jailbreakApps, PhoneSecurityPaths.jailbreakApps),
            paths(facts, PhoneSecurityCatalog.jailbreakTools, PhoneSecurityPaths.jailbreakTools),
            paths(facts, PhoneSecurityCatalog.rootlessJailbreak, PhoneSecurityPaths.rootlessJailbreak),
            paths(facts, PhoneSecurityCatalog.tweakFrameworks, PhoneSecurityPaths.tweakFrameworks),
            injectedLibraries(facts), dyldEnvironment(facts), sandbox(facts), systemLinks(facts),
            debugger(facts), simulator(facts), osVersion(facts),
            installSource(facts), debugBuild(facts), dataProtection(facts), transportSecurity(facts),
            relayHTTPS(facts), screenCapture(facts), httpProxy(facts), vpn(facts),
        ] + PhonePermission.allCases.map { permission($0, facts) }
        return sorted(findings)
    }

    public static func sorted(_ findings: [PhoneSecurityFinding]) -> [PhoneSecurityFinding] {
        findings.sorted { lhs, rhs in
            if lhs.status.rank != rhs.status.rank { return lhs.status.rank < rhs.status.rank }
            return lhs.check.title.lowercased() < rhs.check.title.lowercased()
        }
    }

    static let passcodeSteps = PhoneSecurityFix(
        label: "How to turn it on", description: "Settings → Face ID & Passcode → Turn Passcode On.",
        action: .steps("Settings → Face ID & Passcode → Turn Passcode On"))

    static func unavailable(_ check: PhoneSecurityCheck, _ detail: String) -> PhoneSecurityFinding {
        PhoneSecurityFinding(check: check, status: .unavailable, detail: detail)
    }

    static func passcode(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.passcode
        guard let set = facts.passcodeSet else { return unavailable(check, "Could not read whether a passcode is set.") }
        if set { return PhoneSecurityFinding(check: check, status: .pass, detail: "A passcode is set.") }
        return PhoneSecurityFinding(check: check, status: .critical, detail: "No passcode is set.", fix: passcodeSteps)
    }

    static func biometrics(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.biometrics
        guard let biometry = facts.biometry else { return unavailable(check, "Could not read the biometric sensor.") }
        if biometry == .none { return PhoneSecurityFinding(check: check, status: .pass, detail: "This iPhone has no Face ID or Touch ID.") }
        if facts.passcodeSet == false {
            return PhoneSecurityFinding(
                check: check, status: .review, detail: "\(biometry.label) needs a passcode first.", fix: passcodeSteps)
        }
        guard let enrolled = facts.biometryEnrolled else { return unavailable(check, "Could not read whether \(biometry.label) is set up.") }
        if enrolled { return PhoneSecurityFinding(check: check, status: .pass, detail: "\(biometry.label) is set up.") }
        let page: String = biometry == .touchID ? "Touch ID & Passcode" : "\(biometry.label) & Passcode"
        let steps: String = "Settings → \(page) → Set Up \(biometry.label)"
        return PhoneSecurityFinding(
            check: check, status: .review, detail: "\(biometry.label) is not set up.",
            fix: PhoneSecurityFix(label: "How to set it up", description: "\(steps).", action: .steps(steps)))
    }

    static func notificationPreviews(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.notificationPreviews
        guard let previews = facts.notificationPreviews else { return unavailable(check, "Could not read ForkMesh's notification settings.") }
        switch previews {
        case .always:
            return PhoneSecurityFinding(
                check: check, status: .review, detail: "ForkMesh previews show even when the phone is locked.",
                fix: PhoneSecurityFix(
                    label: "Open notification settings", description: "Set Show Previews to When Unlocked.",
                    action: .open(.appNotifications)))
        case .whenAuthenticated:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Previews show only when the phone is unlocked.")
        case .never:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Previews never show.")
        case .notificationsOff:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Notifications are off for ForkMesh.")
        }
    }

    public static func found(_ candidates: [String], in present: Set<String>) -> [String] {
        candidates.filter { present.contains($0) }
    }

    static func paths(_ facts: PhoneSecurityFacts, _ check: PhoneSecurityCheck, _ candidates: [String]) -> PhoneSecurityFinding {
        guard let present = facts.existingPaths else { return unavailable(check, "Could not look for files outside the app.") }
        let hits: [String] = found(candidates, in: present)
        if hits.isEmpty {
            return PhoneSecurityFinding(check: check, status: .pass, detail: "None found in \(candidates.count) places checked.")
        }
        return PhoneSecurityFinding(
            check: check, status: .critical, detail: "Found \(hits.joined(separator: ", ")).",
            usage: "These files exist only on a jailbroken iPhone. Restore the phone in Finder or iTunes to remove the jailbreak.")
    }

    public static func injectedImages(_ images: [String]) -> [String] {
        var seen: Set<String> = []
        var out: [String] = []
        for image in images {
            let name: String = (image as NSString).lastPathComponent
            let lower: String = name.lowercased()
            guard PhoneSecurityPaths.injectedImageMarkers.contains(where: { lower.contains($0) }) else { continue }
            if seen.insert(name).inserted { out.append(name) }
        }
        return out
    }

    static func injectedLibraries(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.injectedLibraries
        guard let images = facts.loadedImages else { return unavailable(check, "Could not list the libraries loaded into ForkMesh.") }
        let hits: [String] = injectedImages(images)
        if hits.isEmpty {
            return PhoneSecurityFinding(check: check, status: .pass, detail: "No injected libraries among \(images.count) loaded.")
        }
        let noun: String = hits.count == 1 ? "library is" : "libraries are"
        return PhoneSecurityFinding(
            check: check, status: .critical, detail: "\(hits.count) injected \(noun) loaded into ForkMesh.",
            usage: "Loaded: \(hits.joined(separator: ", ")).")
    }

    public static func injectionVariables(_ environment: [String: String]) -> [String] {
        PhoneSecurityPaths.injectionVariables.filter { !(environment[$0] ?? "").trimmingCharacters(in: .whitespaces).isEmpty }
    }

    static func dyldEnvironment(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.dyldEnvironment
        guard let environment = facts.environment else { return unavailable(check, "Could not read ForkMesh's environment.") }
        let set: [String] = injectionVariables(environment)
        if set.isEmpty { return PhoneSecurityFinding(check: check, status: .pass, detail: "No library injection variables are set.") }
        let usage: String = set.map { "\($0)=\(String(environment[$0]!.prefix(120)))" }.joined(separator: "; ")
        if facts.developing {
            return PhoneSecurityFinding(
                check: check, status: .review, detail: "Set by Xcode in this development run: \(set.joined(separator: ", ")).", usage: usage)
        }
        return PhoneSecurityFinding(check: check, status: .critical, detail: "Set: \(set.joined(separator: ", ")).", usage: usage)
    }

    static func sandbox(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.sandbox
        guard let escaped = facts.canWriteOutsideSandbox else { return unavailable(check, "Could not test the app sandbox.") }
        if escaped { return PhoneSecurityFinding(check: check, status: .critical, detail: "ForkMesh could write outside its sandbox.") }
        return PhoneSecurityFinding(check: check, status: .pass, detail: "ForkMesh cannot write outside its sandbox.")
    }

    static func systemLinks(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.systemLinks
        guard let links = facts.symbolicLinks else { return unavailable(check, "Could not read the system folders.") }
        let hits: [String] = found(PhoneSecurityPaths.systemLinks, in: links)
        if hits.isEmpty {
            return PhoneSecurityFinding(
                check: check, status: .pass, detail: "None of \(PhoneSecurityPaths.systemLinks.count) system folders is a link.")
        }
        return PhoneSecurityFinding(check: check, status: .critical, detail: "Replaced by links: \(hits.joined(separator: ", ")).")
    }

    static func debugger(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.debugger
        guard let attached = facts.debuggerAttached else { return unavailable(check, "Could not read whether a debugger is attached.") }
        if !attached { return PhoneSecurityFinding(check: check, status: .pass, detail: "No debugger is attached.") }
        if facts.debugBuild == true {
            return PhoneSecurityFinding(check: check, status: .review, detail: "A debugger is attached to this debug build.")
        }
        return PhoneSecurityFinding(
            check: check, status: .warning, detail: "A debugger is attached to ForkMesh.",
            usage: "If you are not developing ForkMesh, close it and restart the phone.")
    }

    static func simulator(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.simulator
        guard let simulator = facts.simulator else { return unavailable(check, "Could not read the device type.") }
        if simulator { return PhoneSecurityFinding(check: check, status: .review, detail: "ForkMesh is running in the iOS Simulator.") }
        return PhoneSecurityFinding(check: check, status: .pass, detail: "Running on a real iPhone.")
    }

    public static func majorVersion(_ version: String?) -> Int? {
        guard let first = version?.trimmingCharacters(in: .whitespaces).split(separator: ".").first else { return nil }
        guard let major = Int(first), major > 0 else { return nil }
        return major
    }

    static func osVersion(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.osVersion
        guard let major = majorVersion(facts.osVersion), let version = facts.osVersion?.trimmingCharacters(in: .whitespaces) else {
            return unavailable(check, "Could not read the iOS version.")
        }
        let newest: Int = PhoneSecurityPolicy.newestIOSMajor
        let update = PhoneSecurityFix(
            label: "How to update", description: "Settings → General → Software Update.",
            action: .steps("Settings → General → Software Update"))
        if major >= newest { return PhoneSecurityFinding(check: check, status: .pass, detail: "iOS \(version) is current.") }
        let usage: String = "If this iPhone cannot install iOS \(newest), it no longer gets every security fix."
        if major == newest - 1 {
            return PhoneSecurityFinding(
                check: check, status: .review, detail: "iOS \(version) is one version behind iOS \(newest).", usage: usage, fix: update)
        }
        return PhoneSecurityFinding(
            check: check, status: .warning, detail: "iOS \(version) is \(newest - major) versions behind iOS \(newest).",
            usage: usage, fix: update)
    }

    static func installSource(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.installSource
        guard let source = facts.installSource else { return unavailable(check, "Could not tell how ForkMesh was installed.") }
        switch source {
        case .appStore: return PhoneSecurityFinding(check: check, status: .pass, detail: "Installed from the App Store.")
        case .testFlight: return PhoneSecurityFinding(check: check, status: .pass, detail: "Installed from TestFlight.")
        case .development:
            return PhoneSecurityFinding(
                check: check, status: .review, detail: "Installed from a development or ad hoc build.",
                usage: "If you did not build or side-load ForkMesh yourself, delete it and install it from the App Store.")
        }
    }

    static func debugBuild(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.debugBuild
        guard let debug = facts.debugBuild else { return unavailable(check, "Could not read the build type.") }
        if debug { return PhoneSecurityFinding(check: check, status: .review, detail: "This is a debug build of ForkMesh.") }
        return PhoneSecurityFinding(check: check, status: .pass, detail: "This is a release build.")
    }

    static func dataProtection(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.dataProtection
        guard let protection = facts.fileProtection else { return unavailable(check, "Could not read ForkMesh's file protection.") }
        switch protection {
        case .complete:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Files are locked whenever the phone is locked.")
        case .completeUnlessOpen:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Files are locked when the phone locks, unless already open.")
        case .completeUntilFirstUserAuthentication:
            return PhoneSecurityFinding(
                check: check, status: .pass, detail: "Files are locked until the phone is first unlocked after a restart.")
        case .none:
            return PhoneSecurityFinding(check: check, status: .warning, detail: "ForkMesh's files have no file protection.")
        }
    }

    static func transportSecurity(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.transportSecurity
        guard let ats = facts.transportSecurity else { return unavailable(check, "Could not read App Transport Security.") }
        if ats.allowsArbitraryLoads {
            return PhoneSecurityFinding(check: check, status: .warning, detail: "App Transport Security allows any connection over HTTP.")
        }
        let domains: [String] = ats.insecureDomains.sorted()
        if !domains.isEmpty {
            return PhoneSecurityFinding(check: check, status: .review, detail: "HTTP is allowed for \(domains.joined(separator: ", ")).")
        }
        return PhoneSecurityFinding(check: check, status: .pass, detail: "Every connection must use HTTPS.")
    }

    public static func isLocalHost(_ host: String) -> Bool {
        var name: String = host.lowercased()
        if name.hasPrefix("["), name.hasSuffix("]") { name = String(name.dropFirst().dropLast()) }
        if name == "localhost" || name.hasSuffix(".localhost") || name.hasSuffix(".local") { return true }
        if name == "::1" { return true }
        let octets: [Substring] = name.split(separator: ".", omittingEmptySubsequences: false)
        return octets.count == 4 && octets[0] == "127" && octets.allSatisfy { UInt8($0) != nil }
    }

    static func relayHTTPS(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.relayHTTPS
        guard let text = facts.relayURL?.trimmingCharacters(in: .whitespaces), !text.isEmpty else {
            return unavailable(check, "No relay address is set.")
        }
        guard let url = URL(string: text), let scheme = url.scheme?.lowercased(), let host = url.host, !host.isEmpty else {
            return unavailable(check, "Could not read the relay address \(text).")
        }
        switch scheme {
        case "https", "wss":
            return PhoneSecurityFinding(check: check, status: .pass, detail: "The relay at \(host) uses an encrypted connection.")
        case "http", "ws":
            let fix = PhoneSecurityFix(
                label: "How to fix", description: "Add the relay again with an https:// address in ForkMesh's relay settings.",
                action: .steps("ForkMesh → Settings → Relay"))
            if isLocalHost(host) {
                return PhoneSecurityFinding(
                    check: check, status: .review, detail: "The relay at \(host) is unencrypted (\(scheme)://) on a local address.", fix: fix)
            }
            return PhoneSecurityFinding(check: check, status: .critical, detail: "The relay at \(host) is unencrypted (\(scheme)://).", fix: fix)
        default:
            return unavailable(check, "The relay address \(text) is not a web address.")
        }
    }

    static func screenCapture(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.screenCapture
        guard let captured = facts.screenCaptured else { return unavailable(check, "Could not read whether the screen is captured.") }
        if captured {
            return PhoneSecurityFinding(
                check: check, status: .warning, detail: "The screen is being recorded, mirrored or shared.",
                usage: "Stop the recording in Control Center if you did not start it.")
        }
        return PhoneSecurityFinding(check: check, status: .pass, detail: "The screen is not being recorded or mirrored.")
    }

    static func httpProxy(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.httpProxy
        guard let proxy = facts.proxy else { return unavailable(check, "Could not read the proxy settings.") }
        if proxy.isEmpty { return PhoneSecurityFinding(check: check, status: .pass, detail: "No proxy is set.") }
        let steps: String = "Settings → Wi-Fi → ⓘ next to your network → Configure Proxy → Off"
        return PhoneSecurityFinding(
            check: check, status: .review, detail: "Web traffic goes through a proxy.",
            usage: "Proxy: \(proxy.summary). Keep it only if you or your organisation set it up.",
            fix: PhoneSecurityFix(label: "How to turn it off", description: "\(steps).", action: .steps(steps)))
    }

    static func vpn(_ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.vpn
        guard let interfaces = facts.vpnInterfaces else { return unavailable(check, "Could not read the network interfaces.") }
        if interfaces.isEmpty { return PhoneSecurityFinding(check: check, status: .pass, detail: "No VPN is on.") }
        return PhoneSecurityFinding(check: check, status: .pass, detail: "A VPN is on (\(interfaces.joined(separator: ", "))).")
    }

    static func permission(_ permission: PhonePermission, _ facts: PhoneSecurityFacts) -> PhoneSecurityFinding {
        let check: PhoneSecurityCheck = PhoneSecurityCatalog.permission(permission)
        let name: String = check.title.lowercased()
        guard let state = facts.permissions[permission] else { return unavailable(check, "Could not read ForkMesh's \(name).") }
        let use: String = PhoneSecurityCatalog.permissionUse(permission)
        switch state {
        case .allowed:
            let photos: Bool = permission == .photos
            return PhoneSecurityFinding(
                check: check, status: .review,
                detail: photos ? "ForkMesh can see every photo and video." : "ForkMesh has \(name).",
                usage: "\(use) Turn it off if you do not use these.",
                fix: PhoneSecurityFix(
                    label: "Open ForkMesh settings",
                    description: photos ? "Choose Limited Access or None under Photos." : "Turn off \(check.title) if you do not need it.",
                    action: .open(.app)))
        case .limited:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "ForkMesh can see only the items you picked.", usage: use)
        case .denied:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Turned off for ForkMesh.", usage: use)
        case .restricted:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "Blocked by Screen Time or a device profile.", usage: use)
        case .notAsked:
            return PhoneSecurityFinding(check: check, status: .pass, detail: "ForkMesh has not asked for it.", usage: use)
        }
    }
}

public struct PhoneSecurityReport: Equatable, Sendable {
    public let findings: [PhoneSecurityFinding]

    public init(findings: [PhoneSecurityFinding]) {
        self.findings = PhoneSecurityScanner.sorted(findings)
    }

    public init(facts: PhoneSecurityFacts) {
        findings = PhoneSecurityScanner.scan(facts)
    }

    public static func matches(_ finding: PhoneSecurityFinding, filter: PhoneSecurityFilter) -> Bool {
        switch filter {
        case .all: return true
        case .attention: return finding.status.needsAttention
        case .critical: return finding.status == .critical
        case .warning: return finding.status == .warning
        case .review: return finding.status == .review
        case .fixable: return finding.canFix
        case .pass: return finding.status == .pass
        case .unavailable: return finding.status == .unavailable
        }
    }

    public static func matches(_ finding: PhoneSecurityFinding, query: String) -> Bool {
        let needle: String = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if needle.isEmpty { return true }
        let fields: [String] = [
            finding.check.title, finding.check.category.label, finding.status.label, finding.detail, finding.usage ?? "",
        ]
        return fields.contains { $0.lowercased().contains(needle) }
    }

    public func visible(filter: PhoneSecurityFilter = .all, category: PhoneSecurityCategory? = nil, query: String = "")
        -> [PhoneSecurityFinding]
    {
        findings.filter { finding in
            Self.matches(finding, filter: filter) && (category == nil || finding.check.category == category)
                && Self.matches(finding, query: query)
        }
    }

    public func count(_ filter: PhoneSecurityFilter) -> Int { findings.filter { Self.matches($0, filter: filter) }.count }

    public func count(status: PhoneSecurityStatus) -> Int { findings.filter { $0.status == status }.count }

    public var categories: [PhoneSecurityCategory] {
        PhoneSecurityCategory.allCases.filter { category in findings.contains { $0.check.category == category } }
    }

    public var summary: String {
        "\(findings.count) checks: \(count(status: .pass)) pass, \(count(status: .review)) need review, "
            + "\(count(status: .warning)) warnings, \(count(status: .critical)) critical, "
            + "\(count(status: .unavailable)) unavailable"
    }

    public var headline: String {
        let critical: Int = count(status: .critical)
        let warnings: Int = count(status: .warning)
        let review: Int = count(status: .review)
        if critical > 0 { return critical == 1 ? "1 critical problem" : "\(critical) critical problems" }
        if warnings > 0 { return warnings == 1 ? "1 warning" : "\(warnings) warnings" }
        if review > 0 { return review == 1 ? "1 thing to review" : "\(review) things to review" }
        return "No problems found"
    }

    public var worst: PhoneSecurityStatus? {
        findings.map(\.status).filter(\.needsAttention).min { $0.rank < $1.rank }
    }

    public func filteredSummary(filter: PhoneSecurityFilter = .all, category: PhoneSecurityCategory? = nil, query: String = "")
        -> String
    {
        let needle: String = query.trimmingCharacters(in: .whitespacesAndNewlines)
        var scope: [String] = []
        if filter != .all { scope.append(filter.label) }
        if let category { scope.append(category.label) }
        let scopeText: String = scope.joined(separator: " · ")
        if needle.isEmpty && scope.isEmpty { return summary }
        let matched: Int = visible(filter: filter, category: category, query: query).count
        if matched == 0 {
            if scope.isEmpty { return "No checks match \"\(needle)\". Try a different search." }
            if needle.isEmpty { return "No checks in \(scopeText). Try a different filter." }
            return "No checks in \(scopeText) match \"\(needle)\". Try a different filter or search."
        }
        let description: String
        if scope.isEmpty {
            description = "matching \"\(needle)\""
        } else if needle.isEmpty {
            description = "in \(scopeText)"
        } else {
            description = "in \(scopeText) matching \"\(needle)\""
        }
        return "Showing \(matched) of \(findings.count) checks \(description). \(summary)"
    }
}
