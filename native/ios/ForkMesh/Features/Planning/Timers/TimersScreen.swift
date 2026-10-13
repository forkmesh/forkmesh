import FMApi
import FMBase
import FMPlanning
import SwiftUI
import UserNotifications

@MainActor
final class PlanningTimersRuntime {
    let store: PlanningTimerStore
    var onActivityChange: ((TimerActivitySnapshot?) -> Void)?
    private var scheduled: Set<String> = []

    init(store: PlanningTimerStore) {
        self.store = store
        store.onChange = { [weak self] (_: [PlanningTimer]) in
            self?.reschedule()
        }
    }

    static func shared() -> PlanningTimersRuntime? {
        if let existing = Services.get(PlanningTimersRuntime.self) {
            return existing
        }
        guard let prefs = Services.get(KeyValueStore.self) else {
            return nil
        }
        let runtime: PlanningTimersRuntime = PlanningTimersRuntime(store: PlanningTimerStore(store: prefs))
        Services.put(runtime)
        return runtime
    }

    func appBecameActive() {
        let rung: [PlanningTimer] = store.settle()
        reschedule()
        guard !rung.isEmpty, let api = Services.get(ApiService.self), api.hasSession else {
            return
        }
        let now: Int64 = SystemClock().nowMillis
        Task { _ = await TimerPings.post(rung, client: api, nowMillis: now) }
    }

    func reschedule() {
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        let plan: [PlanningTimerNotification] = store.notificationPlan()
        let wanted: Set<String> = Set(plan.map(\.id))
        let stale: [String] = Array(scheduled.subtracting(wanted))
        if !stale.isEmpty {
            center.removePendingNotificationRequests(withIdentifiers: stale)
        }
        let now: Int64 = SystemClock().nowMillis
        for item in plan {
            let content: UNMutableNotificationContent = UNMutableNotificationContent()
            content.title = item.title
            content.body = item.body
            content.sound = .default
            content.interruptionLevel = .timeSensitive
            content.threadIdentifier = "timers"
            let seconds: TimeInterval = max(1, Double(item.fireAtMs - now) / 1_000)
            let trigger: UNTimeIntervalNotificationTrigger = UNTimeIntervalNotificationTrigger(timeInterval: seconds, repeats: false)
            center.add(UNNotificationRequest(identifier: item.id, content: content, trigger: trigger))
        }
        scheduled = wanted
        ShellCounts.shared.timers = store.timers.filter(\.isRunning).count
        onActivityChange?(store.activityTimer().map { store.activitySnapshot($0) })
    }

    func requestPermission() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { (_: Bool, _: Error?) in }
    }
}

struct TimersScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @State private var runtime: PlanningTimersRuntime? = PlanningTimersRuntime.shared()
    @State private var adding: Bool = false
    @State private var revision: Int = 0

    var body: some View {
        Group {
            if let runtime = runtime {
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    TimersList(runtime: runtime, now: context.date.epochMillis, revision: revision, onChange: { revision += 1 })
                }
                .sheet(isPresented: $adding) {
                    TimerAddSheet(runtime: runtime, onAdded: { revision += 1 })
                }
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        Button {
                            runtime.requestPermission()
                            adding = true
                        } label: {
                            Image(systemName: "plus")
                        }
                        .accessibilityLabel("New timer")
                    }
                }
                .onAppear {
                    runtime.appBecameActive()
                    revision += 1
                }
                .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                    if phase == .active {
                        runtime.appBecameActive()
                        revision += 1
                    }
                }
            } else {
                FmEmptyState(icon: "timer_outlined", title: "Timers are unavailable")
            }
        }
        .background(fm.bgBase)
        .navigationTitle("Timers")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct TimersList: View {
    @Environment(\.fmPalette) private var fm
    let runtime: PlanningTimersRuntime
    let now: Int64
    let revision: Int
    let onChange: () -> Void

    var body: some View {
        let timers: [PlanningTimer] = runtime.store.timers
        List {
            if timers.isEmpty {
                Text("No timers. Start a timer, a countdown or an alarm with +.").foregroundStyle(fm.textSecondary)
            }
            ForEach(timers) { (timer: PlanningTimer) in
                TimerRow(timer: timer, now: now, runtime: runtime, onChange: onChange)
            }
            if timers.contains(where: \.isFinished) {
                Button("Clear finished", role: .destructive) {
                    runtime.store.removeFinished()
                    onChange()
                }
            }
        }
        .listStyle(.insetGrouped)
        .id(revision)
    }
}

struct TimerRow: View {
    @Environment(\.fmPalette) private var fm
    let timer: PlanningTimer
    let now: Int64
    let runtime: PlanningTimersRuntime
    let onChange: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: timer.kind.systemImage)
                .foregroundStyle(timer.isRunning ? fm.accent : fm.textSecondary)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(timer.label)
                Text(timer.kind == .alarm ? PlanningFormat.time(timer.alarmAtMs, zone: .current) : timer.kind.label)
                    .font(.caption)
                    .foregroundStyle(fm.textSecondary)
            }
            Spacer()
            Text(timer.display(now: now))
                .font(.title3.monospacedDigit())
                .foregroundStyle(timer.isFinished ? fm.textTertiary : fm.textPrimary)
        }
        .accessibilityElement(children: .combine)
        .swipeActions(edge: .trailing) {
            Button("Delete", role: .destructive) {
                runtime.store.remove(timer.id)
                onChange()
            }
        }
        .swipeActions(edge: .leading) {
            if timer.isRunning && timer.kind != .alarm {
                Button("Pause") {
                    runtime.store.pause(timer.id)
                    onChange()
                }
                .tint(fm.warning)
            } else if timer.isPaused {
                Button("Resume") {
                    runtime.store.resume(timer.id)
                    onChange()
                }
                .tint(fm.success)
            }
            if timer.kind != .alarm {
                Button("Restart") {
                    runtime.store.restart(timer.id)
                    onChange()
                }
                .tint(fm.accent)
            }
        }
        .contextMenu {
            if timer.kind == .stopwatch && !timer.isFinished {
                Button("Stop") {
                    runtime.store.stop(timer.id)
                    onChange()
                }
            }
        }
    }
}

struct TimerAddSheet: View {
    @Environment(\.dismiss) private var dismiss
    let runtime: PlanningTimersRuntime
    let onAdded: () -> Void
    @State private var kind: PlanningTimerKind = .countdown
    @State private var label: String = ""
    @State private var minutes: Int = 10
    @State private var seconds: Int = 0
    @State private var alarmAt: Date = Date().addingTimeInterval(3_600)
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Picker("Kind", selection: $kind) {
                    ForEach(PlanningTimerKind.allCases, id: \.self) { (value: PlanningTimerKind) in
                        Text(value.label).tag(value)
                    }
                }
                .pickerStyle(.segmented)
                TextField("Label", text: $label)
                switch kind {
                case .countdown:
                    Stepper("\(minutes) min", value: $minutes, in: 0...1_440)
                    Stepper("\(seconds) s", value: $seconds, in: 0...59, step: 5)
                case .alarm:
                    DatePicker("Rings at", selection: $alarmAt, in: Date()...)
                case .stopwatch:
                    Text("Counts up from now.").foregroundStyle(.secondary)
                }
                if let error = error {
                    Text(error).foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            .navigationTitle("New \(kind.label.lowercased())")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Start") { start() }
                }
            }
        }
    }

    private func start() {
        do {
            switch kind {
            case .stopwatch: try runtime.store.startStopwatch(label: label)
            case .countdown: try runtime.store.startCountdown(label: label, seconds: minutes * 60 + seconds)
            case .alarm: try runtime.store.setAlarm(label: label, atMs: alarmAt.epochMillis)
            }
            onAdded()
            dismiss()
        } catch {
            self.error = "\(error)"
        }
    }
}
