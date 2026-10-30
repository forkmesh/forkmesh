import FMBase
import Foundation

public enum CallDiagnosisSeverity: String, Equatable, Sendable {
    case ok
    case warn
    case bad
    case unknown
}

public struct CallDiagnosisFix: Equatable, Sendable, Identifiable {
    public let id: String
    public let text: String
    public var worked: Int
    public var failed: Int

    public init(id: String, text: String, worked: Int = 0, failed: Int = 0) {
        self.id = id
        self.text = text
        self.worked = worked
        self.failed = failed
    }
}

public struct CallDiagnosisIssue: Equatable, Sendable, Identifiable {
    public let id: String
    public let severity: CallDiagnosisSeverity
    public let title: String
    public let area: String
    public let cause: String
    public let detail: String
    public let fixes: [CallDiagnosisFix]
}

public struct CallDiagnosisMetrics: Equatable, Sendable {
    public var samples = 0
    public var rttMs: Double?
    public var lossPct: Double?
    public var jitterMs: Double?
    public var sendKbps: Double?
    public var recvKbps: Double?
    public var outFps: Double?
    public var outHeight: Double?
    public var inFps: Double?
    public var inHeight: Double?
    public var faceMs: Double?
    public var faceFps: Double?
    public var faceLevel: Double?

    public init() {}

    public var entries: [(String, Double?)] {
        [
            ("rttMs", rttMs), ("lossPct", lossPct), ("jitterMs", jitterMs), ("sendKbps", sendKbps),
            ("recvKbps", recvKbps), ("outFps", outFps), ("outHeight", outHeight), ("inFps", inFps),
            ("inHeight", inHeight), ("faceMs", faceMs), ("faceFps", faceFps), ("faceLevel", faceLevel),
        ]
    }
}

public struct CallDiagnosis: Equatable, Sendable {
    public let at: Double?
    public let severity: CallDiagnosisSeverity
    public let issues: [CallDiagnosisIssue]
    public let metrics: CallDiagnosisMetrics

    public static let empty = CallDiagnostics.diagnose([])

    public var problem: Bool { severity == .warn || severity == .bad }

    public var summary: String {
        let count = issues.count
        if count > 0 {
            let names = issues.map { $0.title.lowercased() }.joined(separator: ", ")
            return "\(count) problem\(count == 1 ? "" : "s") found: \(names)"
        }
        return severity == .ok ? "No problems found" : "Checking the call"
    }

    public var heading: String {
        let count = issues.count
        if count > 0 { return "\(count) problem\(count == 1 ? "" : "s") found" }
        return severity == .ok ? "No problems found" : "Not enough measurements yet"
    }
}

public struct CallDiagnosticsReport: Equatable, Sendable {
    public var at: Double
    public var issues: [String]
    public var metrics: JSONObject

    public init(at: Double, issues: [String], metrics: JSONObject) {
        self.at = at
        self.issues = issues
        self.metrics = metrics
    }
}

public struct CallFixRecord: Equatable, Sendable {
    public var worked: Int
    public var failed: Int

    public init(worked: Int = 0, failed: Int = 0) {
        self.worked = worked
        self.failed = failed
    }
}

public struct CallDiagnosticsMemory: Equatable, Sendable {
    public var reports: [CallDiagnosticsReport] = []
    public var fixes: [String: CallFixRecord] = [:]
    public var notes: [String] = []

    public init() {}

    public init(json text: String?) {
        guard let text, let raw = try? JSON.parse(text), let object = raw.objectValue else { return }
        if let list = object["reports"]?.arrayValue {
            reports = list.suffix(CallDiagnostics.reports).map { (value: JSONValue) in
                let entry = value.objectOrEmpty
                return CallDiagnosticsReport(
                    at: entry["at"]?.doubleValue ?? 0, issues: entry.strings("issues"), metrics: entry.obj("metrics"))
            }
        }
        if let records = object["fixes"]?.objectValue {
            for (key, value) in records {
                let entry = value.objectOrEmpty
                fixes[key] = CallFixRecord(
                    worked: Int(entry["worked"]?.doubleValue ?? 0), failed: Int(entry["failed"]?.doubleValue ?? 0))
            }
        }
        if let list = object["notes"]?.arrayValue {
            notes = Array(list.compactMap(\.stringValue).suffix(CallDiagnostics.notes))
        }
    }

    public var json: String {
        var records = JSONObject()
        for key in fixes.keys.sorted() {
            let record = fixes[key] ?? CallFixRecord()
            records[key] = .object(["worked": .int(Int64(record.worked)), "failed": .int(Int64(record.failed))])
        }
        let list: [JSONValue] = reports.map { (report: CallDiagnosticsReport) in
            .object([
                "at": CallDiagnostics.number(report.at), "issues": .array(report.issues.map { .string($0) }),
                "metrics": .object(report.metrics),
            ])
        }
        return JSON.encode(
            .object(["reports": .array(list), "fixes": .object(records), "notes": .array(notes.map { .string($0) })]))
    }

    public func record(_ issue: String, _ fix: String) -> CallFixRecord {
        fixes[issue + ":" + fix] ?? CallFixRecord()
    }
}

public struct CallDiagnosticsEnvironment: Equatable, Sendable {
    public var userAgent: String
    public var cores: Int
    public var memory: Double
    public var connection: String

    public init(userAgent: String = "", cores: Int = 0, memory: Double = 0, connection: String = "") {
        self.userAgent = userAgent
        self.cores = cores
        self.memory = memory
        self.connection = connection
    }
}

public enum CallDiagnostics {
    public static let key = "fm.call.diagnostics"
    public static let window = 8
    public static let minSamples = 3
    public static let reports = 20
    public static let notes = 8
    public static let noteChars = 280
    public static let videoMinFps: Double = 15
    public static let videoMinHeight: Double = 360
    public static let videoMinKbps: Double = 100
    public static let faceSlowMs: Double = 34
    public static let faceBadMs: Double = 50

    public static let fixes: [String: String] = [
        "wired": "Use a wired connection, or move closer to the Wi-Fi router.",
        "traffic": "Pause downloads, uploads, cloud backups and video streaming on this network.",
        "vpn": "Turn off a VPN or proxy for the call.",
        "relay": "If the call says Relayed, this network blocks direct connections: try another network, or allow UDP on this one.",
        "band": "Use 5 GHz Wi-Fi instead of 2.4 GHz, away from microwaves and Bluetooth speakers.",
        "router": "Restart the Wi-Fi router.",
        "camera": "Turn your camera off for a minute so your voice gets the whole connection.",
        "tabs": "Close other tabs and apps, especially other calls, games and screen recorders.",
        "filters": "Turn off the face, background or outfit filter.",
        "power": "Plug the device in and turn off battery saver.",
        "hardware": "Turn on hardware acceleration in the browser's settings and restart the browser.",
        "light": "Add light in front of you: cameras lower their frame rate in the dark.",
        "cameraApps": "Close other apps that use the camera.",
        "theirSide": "Ask them to press their diagnostics button: poor incoming video is usually their connection or device.",
        "background": "Choose None or Blur as the background instead of a picture.",
        "faceOff": "Turn the face off when the call needs to be smooth.",
    ]

    struct Kind {
        let title: String
        let area: String
        let cause: String
        let fixes: [String]
    }

    static let kinds: [String: Kind] = [
        "latency": Kind(
            title: "High latency", area: "network",
            cause: "Sound and video take a long time to arrive, so you talk over each other.",
            fixes: ["wired", "traffic", "vpn", "relay"]),
        "loss": Kind(
            title: "Packet loss", area: "network",
            cause: "Parts of the call are lost on the way, so voice drops out and video freezes or blurs.",
            fixes: ["wired", "band", "traffic", "router"]),
        "jitter": Kind(
            title: "Unsteady connection", area: "network",
            cause: "Packets arrive unevenly, so the call buffers more and voice can sound choppy.",
            fixes: ["traffic", "wired", "band", "vpn"]),
        "bandwidth": Kind(
            title: "Upload too slow for your video", area: "encoder",
            cause: "The browser is lowering your video quality because the upload cannot keep up.",
            fixes: ["traffic", "wired", "camera", "filters"]),
        "cpu": Kind(
            title: "Device too busy", area: "encoder",
            cause: "The browser is lowering your video quality because the processor cannot encode it in time.",
            fixes: ["tabs", "filters", "power", "hardware"]),
        "sendFps": Kind(
            title: "Your video is choppy", area: "camera", cause: "Your video leaves this device at a low frame rate.",
            fixes: ["light", "cameraApps", "filters", "power"]),
        "receive": Kind(
            title: "Their video is poor", area: "network",
            cause: "The other side's video arrives small or at a low frame rate.", fixes: ["theirSide", "traffic", "wired"]),
        "face": Kind(
            title: "Face filter is slow", area: "face filter",
            cause: "The face filter needs too long for each frame, so your face lags behind you.",
            fixes: ["tabs", "power", "hardware", "background", "faceOff"]),
    ]

    static func median(_ values: [Double]) -> Double? {
        guard !values.isEmpty else { return nil }
        let sorted = values.sorted()
        let middle = sorted.count >> 1
        return sorted.count % 2 == 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2
    }

    static func mean(_ values: [Double]) -> Double? {
        values.isEmpty ? nil : values.reduce(0, +) / Double(values.count)
    }

    public static func round(_ value: Double) -> Int {
        let floor = value.rounded(.down)
        return Int(value - floor >= 0.5 ? floor + 1 : floor)
    }

    public static func fixed(_ value: Double, _ digits: Int) -> String {
        guard value.isFinite else { return value.isNaN ? "NaN" : (value < 0 ? "-Infinity" : "Infinity") }
        let negative = value < 0
        let exact = String(format: "%.90f", abs(value))
        let parts = exact.split(separator: ".", maxSplits: 1).map(String.init)
        var whole = Array(parts[0])
        let fraction = Array(parts.count > 1 ? parts[1] : "")
        var kept = Array(fraction.prefix(digits))
        while kept.count < digits { kept.append("0") }
        let next = digits < fraction.count ? fraction[digits] : "0"
        if next >= "5" {
            var carry = true
            var index = kept.count - 1
            while carry && index >= 0 {
                if kept[index] == "9" {
                    kept[index] = "0"
                } else {
                    kept[index] = Character(String((kept[index].wholeNumberValue ?? 0) + 1))
                    carry = false
                }
                index -= 1
            }
            index = whole.count - 1
            while carry && index >= 0 {
                if whole[index] == "9" {
                    whole[index] = "0"
                } else {
                    whole[index] = Character(String((whole[index].wholeNumberValue ?? 0) + 1))
                    carry = false
                }
                index -= 1
            }
            if carry { whole.insert("1", at: 0) }
        }
        var out = negative ? "-" : ""
        out += String(whole)
        if digits > 0 { out += "." + String(kept) }
        return out
    }

    public static func text(_ value: Double) -> String {
        let formatted = JSON.formatDouble(value)
        return formatted.hasSuffix(".0") ? String(formatted.dropLast(2)) : formatted
    }

    static func number(_ value: Double) -> JSONValue {
        if value.rounded() == value, abs(value) < 9e15 { return .int(Int64(value)) }
        return .double(value)
    }

    public static func formatKbps(_ kbps: Double?) -> String {
        guard let kbps else { return "" }
        if kbps >= 1000 { return fixed(kbps / 1000, 1) + " Mb/s" }
        return "\(round(kbps)) kb/s"
    }

    static func describeOut(_ sample: CallQualitySample, fps: Double?, kbps: Double?) -> String {
        var parts: [String] = []
        if let width = sample.outWidth, let height = sample.outHeight, width != 0, height != 0 {
            parts.append("\(width)×\(height)")
        }
        if let fps { parts.append("\(round(fps)) fps") }
        let speed = formatKbps(kbps)
        if !speed.isEmpty { parts.append(speed) }
        return parts.joined(separator: " · ")
    }

    static func make(_ id: String, _ severity: CallDiagnosisSeverity, _ detail: String) -> CallDiagnosisIssue {
        let kind = kinds[id] ?? Kind(title: id, area: "", cause: "", fixes: [])
        return CallDiagnosisIssue(
            id: id, severity: severity, title: kind.title, area: kind.area, cause: kind.cause, detail: detail,
            fixes: kind.fixes.map { CallDiagnosisFix(id: $0, text: fixes[$0] ?? "") })
    }

    public static func diagnose(
        _ samples: [CallQualitySample], video: Bool = false, sending: Bool = true, window: Int = CallDiagnostics.window
    ) -> CallDiagnosis {
        let last = Array(samples.suffix(max(0, window)))
        func pick(_ key: KeyPath<CallQualitySample, Double?>) -> [Double] {
            last.compactMap { $0[keyPath: key] }.filter(\.isFinite)
        }
        let outHeights = last.compactMap { $0.outHeight.map(Double.init) }
        let inHeights = last.compactMap { $0.inHeight.map(Double.init) }
        let need = min(minSamples, window)
        var metrics = CallDiagnosisMetrics()
        metrics.samples = last.count
        metrics.rttMs = median(pick(\.rttMs))
        metrics.lossPct = mean(pick(\.lossPct))
        metrics.jitterMs = median(pick(\.jitterMs))
        metrics.sendKbps = median(pick(\.sendKbps))
        metrics.recvKbps = median(pick(\.recvKbps))
        metrics.outFps = median(pick(\.outFps))
        metrics.outHeight = median(outHeights)
        metrics.inFps = median(pick(\.inFps))
        metrics.inHeight = median(inHeights)
        metrics.faceMs = median(pick(\.faceMs))
        metrics.faceFps = median(pick(\.faceFps))
        metrics.faceLevel = last.last?.faceLevel
        var issues: [CallDiagnosisIssue] = []
        let rttSteps = CallQuality.rttSteps
        let lossSteps = CallQuality.lossSteps
        let jitterSteps = CallQuality.jitterSteps
        if pick(\.rttMs).count >= need, let rtt = metrics.rttMs, rtt >= rttSteps[1] {
            issues.append(make(
                "latency", rtt >= rttSteps[2] ? .bad : .warn,
                "Round trip \(round(rtt)) ms; under \(text(rttSteps[0])) ms feels natural."))
        }
        if pick(\.lossPct).count >= need, let loss = metrics.lossPct, loss >= lossSteps[1] {
            issues.append(make(
                "loss", loss >= lossSteps[2] ? .bad : .warn,
                "\(fixed(loss, 1))% of packets lost; under \(text(lossSteps[0]))% is fine."))
        }
        if pick(\.jitterMs).count >= need, let jitter = metrics.jitterMs, jitter >= jitterSteps[1] {
            issues.append(make(
                "jitter", jitter >= jitterSteps[2] ? .bad : .warn,
                "\(round(jitter)) ms jitter; under \(text(jitterSteps[0])) ms is fine."))
        }
        if video && sending && last.count >= need, let newest = last.last {
            func limited(_ reason: String) -> Bool { last.filter { $0.limit == reason }.count * 2 > last.count }
            let described = describeOut(newest, fps: metrics.outFps, kbps: metrics.sendKbps)
            let out = described.isEmpty ? "reduced" : described
            if limited("bandwidth") { issues.append(make("bandwidth", .warn, "Video out is \(out), limited by bandwidth.")) }
            if limited("cpu") { issues.append(make("cpu", .warn, "Video out is \(out), limited by CPU.")) }
            if pick(\.outFps).count >= need, let fps = metrics.outFps, fps < videoMinFps {
                issues.append(make(
                    "sendFps", fps < videoMinFps / 2 ? .bad : .warn,
                    "Your video leaves at \(round(fps)) fps; \(text(videoMinFps)) or more looks smooth."))
            }
        }
        if video && pick(\.recvKbps).count >= need, let recv = metrics.recvKbps, recv >= videoMinKbps {
            let slow = pick(\.inFps).count >= need && (metrics.inFps ?? .infinity) < videoMinFps
            let small = inHeights.count >= need && (metrics.inHeight ?? .infinity) < videoMinHeight
            if slow || small {
                var parts: [String] = []
                if small, let height = metrics.inHeight { parts.append("\(round(height))p") }
                if slow, let fps = metrics.inFps { parts.append("\(round(fps)) fps") }
                issues.append(make("receive", .warn, "Their video arrives at \(parts.joined(separator: ", "))."))
            }
        }
        if pick(\.faceMs).count >= need, let face = metrics.faceMs, face > faceSlowMs {
            let fps = metrics.faceFps.map { " and draws \(round($0)) frames a second" } ?? ""
            issues.append(make(
                "face", face > faceBadMs ? .bad : .warn,
                "The face filter takes \(round(face)) ms a frame\(fps); under \(text(faceSlowMs)) ms keeps up with the camera."))
        }
        let ordered = issues.filter { $0.severity == .bad } + issues.filter { $0.severity != .bad }
        let severity: CallDiagnosisSeverity =
            ordered.contains { $0.severity == .bad }
            ? .bad : !ordered.isEmpty ? .warn : last.count >= need ? .ok : .unknown
        return CallDiagnosis(at: last.last?.at, severity: severity, issues: ordered, metrics: metrics)
    }

    public static func rankFixes(_ issue: CallDiagnosisIssue, _ memory: CallDiagnosticsMemory) -> [CallDiagnosisFix] {
        let scored = issue.fixes.enumerated().map { (index: Int, fix: CallDiagnosisFix) -> (Int, Int, CallDiagnosisFix) in
            let record = memory.record(issue.id, fix.id)
            return (record.worked * 2 - record.failed, index, CallDiagnosisFix(
                id: fix.id, text: fix.text, worked: record.worked, failed: record.failed))
        }
        return scored.sorted { $0.0 != $1.0 ? $0.0 > $1.0 : $0.1 < $1.1 }.map(\.2)
    }

    public static func recordFixOutcome(_ memory: inout CallDiagnosticsMemory, issue: String, fix: String, worked: Bool) {
        var record = memory.record(issue, fix)
        if worked { record.worked += 1 } else { record.failed += 1 }
        memory.fixes[issue + ":" + fix] = record
    }

    public static func rememberReport(
        _ memory: inout CallDiagnosticsMemory, _ diagnosis: CallDiagnosis, now: Double = Date().timeIntervalSince1970
    ) {
        var metrics = JSONObject()
        for (key, value) in diagnosis.metrics.entries {
            guard let value else { continue }
            metrics[key] = number(Double(fixed(value, 1)) ?? value)
        }
        memory.reports.append(
            CallDiagnosticsReport(at: diagnosis.at ?? now, issues: diagnosis.issues.map(\.id), metrics: metrics))
        if memory.reports.count > reports { memory.reports.removeFirst(memory.reports.count - reports) }
    }

    @discardableResult
    public static func rememberNote(_ memory: inout CallDiagnosticsMemory, _ text: String) -> Bool {
        var note = text.replacingOccurrences(
            of: "^\\s*lesson\\s*:\\s*", with: "", options: [.regularExpression, .caseInsensitive])
        note = note.replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if note.utf16.count > noteChars {
            note = String(decoding: Array(note.utf16.prefix(noteChars)), as: UTF16.self)
        }
        guard !note.isEmpty else { return false }
        memory.notes.removeAll { $0 == note }
        memory.notes.append(note)
        if memory.notes.count > notes { memory.notes.removeFirst(memory.notes.count - notes) }
        return true
    }

    public static func describe(_ metrics: CallDiagnosisMetrics) -> [String] {
        var lines: [String] = []
        if let rtt = metrics.rttMs {
            var line = "Network: \(fixed(rtt, 0)) ms round trip"
            if let loss = metrics.lossPct { line += ", \(fixed(loss, 1))% loss" }
            if let jitter = metrics.jitterMs { line += ", \(fixed(jitter, 0)) ms jitter" }
            lines.append(line)
        }
        if metrics.outFps != nil || metrics.sendKbps != nil {
            let parts = [
                metrics.outHeight.map { fixed($0, 0) + "p" } ?? "", metrics.outFps.map { fixed($0, 0) + " fps" } ?? "",
                formatKbps(metrics.sendKbps),
            ].filter { !$0.isEmpty }
            lines.append("Video out: " + parts.joined(separator: ", "))
        }
        if metrics.inFps != nil || metrics.recvKbps != nil {
            let parts = [
                metrics.inHeight.map { fixed($0, 0) + "p" } ?? "", metrics.inFps.map { fixed($0, 0) + " fps" } ?? "",
                formatKbps(metrics.recvKbps),
            ].filter { !$0.isEmpty }
            lines.append("Video in: " + parts.joined(separator: ", "))
        }
        if let face = metrics.faceMs {
            var line = "Face filter: \(fixed(face, 0)) ms a frame"
            if let fps = metrics.faceFps { line += ", \(fixed(fps, 0)) fps" }
            if let level = metrics.faceLevel { line += ", quality step \(text(level))" }
            lines.append(line)
        }
        return lines
    }

    public static func earlierReports(_ diagnosis: CallDiagnosis, _ memory: CallDiagnosticsMemory) -> [CallDiagnosticsReport] {
        memory.reports.filter { diagnosis.at == nil || $0.at != diagnosis.at }
    }

    public static func promptLabel(_ diagnosis: CallDiagnosis, _ memory: CallDiagnosticsMemory) -> String {
        let earlier = earlierReports(diagnosis, memory).count
        return "Prompt for an AI assistant · v\(earlier + 1), learned from \(earlier) earlier report\(earlier == 1 ? "" : "s")"
    }

    public static func prompt(
        _ diagnosis: CallDiagnosis, _ memory: CallDiagnosticsMemory,
        environment: CallDiagnosticsEnvironment = CallDiagnosticsEnvironment()
    ) -> String {
        let earlier = earlierReports(diagnosis, memory)
        var lines: [String] = []
        lines.append(
            "Prompt v\(earlier.count + 1) · learned from \(earlier.count) earlier report\(earlier.count == 1 ? "" : "s") on this device.")
        lines.append("")
        lines.append(
            "You are helping me fix the quality of a ForkMesh call (WebRTC in the browser, with an optional on-device face filter). Measurements from the last few seconds of my call:")
        for line in describe(diagnosis.metrics) { lines.append("- " + line) }
        let device = [
            environment.userAgent, environment.cores != 0 ? "\(environment.cores) CPU cores" : "",
            environment.memory != 0 ? "\(text(environment.memory)) GB memory" : "", environment.connection,
        ].filter { !$0.isEmpty }
        if !device.isEmpty { lines.append("- Device: " + device.joined(separator: "; ")) }
        lines.append("")
        if diagnosis.issues.isEmpty {
            lines.append("No problem was detected automatically. If the call still feels wrong to me, help me find out why.")
        } else {
            lines.append("Problems detected:")
            for (index, issue) in diagnosis.issues.enumerated() {
                let seen = earlier.filter { $0.issues.contains(issue.id) }.count
                lines.append(
                    "\(index + 1). \(issue.title) (\(issue.severity == .bad ? "severe" : "noticeable"), \(issue.area)). \(issue.detail)")
                if seen > 0 { lines.append("   Seen in \(seen) of \(earlier.count) earlier reports on this device.") }
                let ranked = rankFixes(issue, memory)
                let worked = ranked.filter { $0.worked > 0 }
                let failed = ranked.filter { $0.failed > 0 && $0.worked == 0 }
                if !worked.isEmpty {
                    lines.append("   Worked here before: " + worked.map { "\($0.text) (\($0.worked)×)" }.joined(separator: " "))
                }
                if !failed.isEmpty {
                    lines.append(
                        "   Tried here without clearing it: " + failed.map { "\($0.text) (\($0.failed)×)" }.joined(separator: " "))
                }
                lines.append("   Suggested order: " + ranked.map(\.text).joined(separator: " "))
            }
        }
        if !memory.notes.isEmpty {
            lines.append("")
            lines.append("Lessons saved from earlier calls on this device (newest last):")
            for note in memory.notes { lines.append("- " + note) }
        }
        lines.append("")
        lines.append("Please:")
        lines.append("1. Say in one or two sentences what most likely causes this, using the numbers above.")
        lines.append(
            "2. Give me the steps to fix it, most likely first. Start from what worked here before, and skip what was tried without clearing it unless something has changed.")
        lines.append(
            "3. If it looks like a ForkMesh bug rather than my network or device, say which part (network, camera, encoder or face filter) and what to look for in chrome://webrtc-internals.")
        lines.append(
            "4. End with one line that starts with \"LESSON:\" saying what to try first next time on this device. I will save it so the next version of this prompt starts from it.")
        return lines.joined(separator: "\n")
    }

    public static func fixLabel(_ fix: CallDiagnosisFix) -> String {
        fix.text + (fix.worked > 0 ? " (worked here before)" : fix.failed > 0 ? " (tried here before)" : "")
    }
}

public struct CallDiagnosticsTracker: Equatable, Sendable {
    public private(set) var pending: [(issue: String, fix: String)] = []

    public init() {}

    public static func == (lhs: CallDiagnosticsTracker, rhs: CallDiagnosticsTracker) -> Bool {
        lhs.pending.map { $0.issue + ":" + $0.fix } == rhs.pending.map { $0.issue + ":" + $0.fix }
    }

    public mutating func opened(_ diagnosis: CallDiagnosis, memory: inout CallDiagnosticsMemory) {
        if let at = diagnosis.at, memory.reports.last?.at != at { CallDiagnostics.rememberReport(&memory, diagnosis) }
        for issue in diagnosis.issues where !pending.contains(where: { $0.issue == issue.id }) {
            if let first = CallDiagnostics.rankFixes(issue, memory).first { pending.append((issue.id, first.id)) }
        }
    }

    @discardableResult
    public mutating func refreshed(_ diagnosis: CallDiagnosis, memory: inout CallDiagnosticsMemory) -> Bool {
        guard diagnosis.severity != .unknown, !pending.isEmpty else { return false }
        let present = Set(diagnosis.issues.map(\.id))
        var learned = false
        pending.removeAll { (entry: (issue: String, fix: String)) in
            guard !present.contains(entry.issue) else { return false }
            CallDiagnostics.recordFixOutcome(&memory, issue: entry.issue, fix: entry.fix, worked: true)
            learned = true
            return true
        }
        return learned
    }

    @discardableResult
    public mutating func finished(memory: inout CallDiagnosticsMemory) -> Bool {
        guard !pending.isEmpty else { return false }
        for entry in pending { CallDiagnostics.recordFixOutcome(&memory, issue: entry.issue, fix: entry.fix, worked: false) }
        pending.removeAll()
        return true
    }
}
