import AVFoundation
import FMAccount
import FMAgents
import FMApi
import FMBase
import FMPackages
import FMTools
import Foundation
import Observation
import QuartzCore
import SwiftUI
import UIKit

@MainActor
final class DjFrameTicker: NSObject {
    weak var probe: DjFrameProbe?

    init(_ probe: DjFrameProbe) {
        self.probe = probe
    }

    @objc func tick(_ link: CADisplayLink) {
        guard probe != nil else {
            link.invalidate()
            return
        }
        let period = link.targetTimestamp - link.timestamp
        let cpu = Double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1e9
        let app = Double(clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID)) / 1e9
        DjVitalsRecorder.shared.update { $0.frame(at: link.timestamp, period: period, cpu: cpu, appCpu: app) }
    }
}

@MainActor
final class DjFrameProbe {
    static let range = CAFrameRateRange(minimum: 30, maximum: 60, preferred: 60)
    static let beforeCommit = 1_998_000
    private var link: CADisplayLink?
    private var watchers: [CFRunLoopObserver] = []

    func start() {
        guard link == nil else { return }
        DjVitalsRecorder.shared.update { $0.begin(at: CACurrentMediaTime()) }
        let link = CADisplayLink(target: DjFrameTicker(self), selector: #selector(DjFrameTicker.tick(_:)))
        link.preferredFrameRateRange = Self.range
        link.add(to: .main, forMode: .common)
        self.link = link
        let woke = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.afterWaiting.rawValue, true, -Int.max) { @Sendable _, _ in
            let now = CACurrentMediaTime()
            DjVitalsRecorder.shared.update { $0.awake(at: now) }
        }
        let committing = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.beforeWaiting.rawValue, true, Self.beforeCommit) { @Sendable _, _ in
            let now = CACurrentMediaTime()
            DjVitalsRecorder.shared.update { $0.committing(at: now) }
        }
        let slept = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.beforeWaiting.rawValue, true, Int.max) { @Sendable _, _ in
            let now = CACurrentMediaTime()
            DjVitalsRecorder.shared.update { $0.asleep(at: now) }
        }
        watchers = [woke, committing, slept].compactMap { $0 }
        for watcher in watchers { CFRunLoopAddObserver(CFRunLoopGetMain(), watcher, .commonModes) }
        Services.get(PerformanceMonitorService.self)?.watchSections { name, start, end in
            DjVitalsRecorder.shared.update { $0.section(name, ms: (end - start) * 1000, at: end) }
        }
    }

    func stop() {
        guard let link else { return }
        link.invalidate()
        self.link = nil
        for watcher in watchers {
            CFRunLoopRemoveObserver(CFRunLoopGetMain(), watcher, .commonModes)
            CFRunLoopObserverInvalidate(watcher)
        }
        watchers = []
        Services.get(PerformanceMonitorService.self)?.watchSections(nil)
        DjVitalsRecorder.shared.update { $0.end(at: CACurrentMediaTime()) }
    }
}

@MainActor
@Observable
final class DjDiagnostics {
    static let shared = DjDiagnostics()
    static let refreshSeconds: Double = 1
    static let scriptSampleEvery = 16

    private(set) var alert: DjVitalsAlert = .none
    private(set) var live = DjVitalsLive()

    func refresh() {
        let now = CACurrentMediaTime()
        let vitals = DjVitalsRecorder.shared.snapshot
        let fresh = vitals.alert(now: now)
        let reading = vitals.live(now: now)
        if fresh != alert { alert = fresh }
        if reading != live { live = reading }
    }

    static func mark(_ text: String) {
        DjVitalsRecorder.shared.mark(text, at: CACurrentMediaTime())
    }

    static func viewed(_ name: String) {
        DjVitalsRecorder.shared.viewed(name)
    }

    func prompt(model: DjStudioModel, includeLogs: Bool) async -> String {
        let now = CACurrentMediaTime()
        let vitals = DjVitalsRecorder.shared.snapshot
        let alert = vitals.alert(now: now)
        let lead = DjVitals.lead(reason: alert.reason)
        let commit = String((AppServices.current?.config.build.commit ?? "").prefix(12))
        var report = "build: \(AppChangelog.label(infoDictionary: Bundle.main.infoDictionary))"
        if !commit.isEmpty { report += ", commit \(commit)" }
        report += "\n" + vitals.report(now: now, alert: alert, context: Self.context(model))
        if let app = await PerformanceReportBuilder.report(router: nil, draft: lead + "\n\n# Diagnostics bundle\n" + report + "\n\n") {
            report += "\n\n" + app
        }
        return DiagnosticsBundle.prompt(lead: lead, report: report, includeLogs: includeLogs, limit: ApiService.maxAgentPromptChars)
    }

    static func context(_ model: DjStudioModel) -> [(String, String)] {
        var out: [(String, String)] = []
        for deck in [model.a, model.b] {
            guard deck.loaded else {
                out.append(("deck \(deck.side.label)", "empty"))
                continue
            }
            var state = deck.playing ? "playing" : deck.coasting ? "coasting" : "stopped"
            if deck.scratching { state += ", scratching" }
            if deck.brake != 0 { state += ", touched" }
            state += ", rate \(String(format: "%.3f", deck.effectiveRate))"
            state += ", \(deck.waveform.count) bars\(deck.reading ? " (reading)" : "")"
            state += ", scratch audio \(deck.scratchAudio == nil ? "no" : "yes")"
            state += ", \(Int(deck.duration.rounded())) s long"
            out.append(("deck \(deck.side.label)", state))
        }
        out.append(("pace", model.pace.label))
        out.append(("crossfader", String(format: "%.2f", model.crossfader)))
        out.append(("automix", (model.automixOn ? "on" : "off") + (model.automixing ? ", fading" : "")))
        out.append(("queue", "\(model.queue.count) videos"))
        if let jam = model.jam {
            out.append((
                "jam", "\(jam.joined ? "joined" : "not joined"), \(jam.members.count) people, motion \(model.shareMotion ? "shared" : "off")\(model.motion.running ? " (sensor on)" : "")"))
        } else {
            out.append(("jam", "none"))
        }
        out.append(("reactions", "\(model.effects.count) playing"))
        if let sheet = model.sheet { out.append(("sheet", sheet.id)) }
        let session = AVAudioSession.sharedInstance()
        out.append((
            "audio session",
            "\(session.category.rawValue.replacingOccurrences(of: "AVAudioSessionCategory", with: "")), \(Int(session.sampleRate)) Hz, buffer \(String(format: "%.1f", session.ioBufferDuration * 1000)) ms, output latency \(String(format: "%.1f", session.outputLatency * 1000)) ms"))
        if let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first {
            out.append(("orientation", scene.interfaceOrientation.isLandscape ? "landscape" : "portrait"))
        }
        let device: Set<String> = ["app", "os", "device", "memory", "thermal state", "low power mode", "display"]
        out += PerformanceReportBuilder.context(nil).filter { device.contains($0.0) }
        return out
    }

    static func send(_ prompt: String) async -> String? {
        guard let composer = Services.get(ShellComposer.self) else {
            return "Open ForkMesh's Home once, then send again."
        }
        if composer.busy { return "Another agent is starting. Try again in a moment." }
        composer.busy = true
        let outcome = await composer.launcher.send(prompt: prompt, repository: DiagnosticsBundle.repository, onDevice: false)
        composer.busy = false
        switch outcome {
        case .queued, .queuedWithoutSession, .desktop, .done:
            FmHaptics.send(.success)
            return nil
        case .device:
            return "Agents for \(DiagnosticsBundle.repository) run on this iPhone, so the report cannot start one here. Send it from a desktop or a mirror."
        case .ignored, .needsFix, .failed:
            return GenieFeature.outcomeLine(outcome)
        }
    }
}

struct DjDiagnosticsButton: View {
    let model: DjStudioModel
    let size: CGFloat
    private var diagnostics: DjDiagnostics { DjDiagnostics.shared }

    var body: some View {
        let _ = DjDiagnostics.viewed("diagnostics button")
        let detected = diagnostics.alert.detected
        Button {
            DjTheme.tick()
            model.sheet = .diagnostics
        } label: {
            Image(systemName: detected ? "exclamationmark.triangle.fill" : "stethoscope")
                .font(.system(size: size * 0.72, weight: .semibold))
                .foregroundStyle(detected ? DjTheme.record : DjTheme.muted.opacity(0.8))
                .frame(width: size, height: size)
                .frame(minWidth: 44, minHeight: 44)
                .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel(detected ? "Studio diagnostics. \(diagnostics.alert.reason)" : "Studio diagnostics")
        .accessibilityIdentifier("dj-diagnostics")
        .task {
            while !Task.isCancelled {
                diagnostics.refresh()
                try? await Task.sleep(for: .seconds(DjDiagnostics.refreshSeconds))
            }
        }
    }
}

struct DjDiagnosticsSheet: View {
    let model: DjStudioModel
    @State private var includeLogs = true
    @State private var preview = ""
    @State private var sending = false
    @State private var problem: String?
    private var diagnostics: DjDiagnostics { DjDiagnostics.shared }

    private static let contents: [String] = [
        "Frame times, dropped frames, hitches, main-thread busy and CPU time while the studio is open",
        "Each long frame with the studio work inside it and what you did just before",
        "How often each part of the studio was redrawn",
        "Player ticks, drift, web view scripts and their round trip",
        "Both decks, crossfader, AUTOMIX, jam and audio session state",
        "App version, iPhone model, iOS version, memory and thermal state",
        "The app's own performance report, if there is room",
    ]

    var body: some View {
        List {
            Section("Now") {
                DjDiagnosticsLive()
            }
            Section("What is sent") {
                ForEach(Self.contents, id: \.self) { (line: String) in
                    Label(line, systemImage: "checkmark")
                }
                Toggle("Include recent log lines", isOn: $includeLogs)
            }
            Section {
                Text("Tokens, emails, phone numbers, link queries and message contents are removed on this iPhone first. A coding agent on \(DiagnosticsBundle.repository) reads the report, fixes the cause on a branch with tests, and records what it learned so the next check starts further ahead.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Section("Preview") {
                DisclosureGroup("Show what is sent") {
                    if preview.isEmpty {
                        ProgressView()
                    } else {
                        Text(verbatim: preview)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                }
            }
            if let problem {
                Section {
                    Text(verbatim: problem)
                        .font(.footnote)
                        .foregroundStyle(DjTheme.record)
                }
            }
            Section {
                Button {
                    submit()
                } label: {
                    HStack {
                        Label("Send to an agent", systemImage: "paperplane.fill")
                        Spacer()
                        if sending { ProgressView() }
                    }
                }
                .disabled(preview.isEmpty || sending)
                .accessibilityIdentifier("dj-diagnostics-send")
            }
        }
        .navigationTitle("Studio diagnostics")
        .task(id: includeLogs) {
            preview = ""
            preview = await diagnostics.prompt(model: model, includeLogs: includeLogs)
        }
    }

    private func submit() {
        let text = preview
        Task { @MainActor in
            sending = true
            problem = nil
            let failure = await DjDiagnostics.send(text)
            sending = false
            if let failure {
                FmHaptics.error()
                problem = failure
                return
            }
            DjDiagnostics.mark("diagnostics sent")
            model.sheet = nil
            model.show("Diagnostics sent. An agent is on it; you'll find it under Agents.")
        }
    }
}

struct DjDiagnosticsLive: View {
    private var diagnostics: DjDiagnostics { DjDiagnostics.shared }

    var body: some View {
        let live = diagnostics.live
        let alert = diagnostics.alert
        VStack(alignment: .leading, spacing: 6) {
            Label {
                Text(alert.detected ? "Problem detected" : live.fps > 0 ? (live.smooth ? "Running smoothly" : "Some stutter") : "Measuring…")
                    .font(.headline)
            } icon: {
                Image(systemName: alert.detected ? "exclamationmark.triangle.fill" : "stethoscope")
                    .foregroundStyle(alert.detected ? DjTheme.record : DjTheme.active)
            }
            if alert.detected {
                Text(verbatim: Self.sentence(alert.reason))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Text(verbatim: "Last \(DjVitals.liveSeconds) s: \(Int(live.fps.rounded())) of \(Int(live.probeHz.rounded())) fps, \(live.dropped) dropped, worst frame \(Int(live.worstMs.rounded())) ms, \(String(format: "%.1f", live.hitchRatio)) ms of hitches a second")
                .font(.system(.footnote, design: .monospaced))
                .foregroundStyle(.secondary)
                .monospacedDigit()
        }
        .padding(.vertical, 2)
        .accessibilityElement(children: .combine)
    }

    static func sentence(_ text: String) -> String {
        guard let first = text.first else { return text }
        return String(first).uppercased() + String(text.dropFirst()) + "."
    }
}
