import FMAccount
import FMApi
import FMBase
import FMTools
import Foundation

enum HealthFeature {
    private static var lifecycleToken: UUID?

    @MainActor
    static var bridge: HealthKitBridge {
        if let existing = Services.get(HealthKitBridge.self) {
            return existing
        }
        let created: HealthKitBridge = Services.put(HealthKitBridge())
        Services.put(created, as: HealthBridge.self)
        return created
    }

    @MainActor
    static var prefs: KeyValueStore {
        Services.get(KeyValueStore.self) ?? Services.put(InMemoryKeyValueStore(), as: KeyValueStore.self)
    }

    @MainActor
    static func register() {
        let health: HealthKitBridge = bridge
        stepsEnabledChanged()
        sleepEnabledChanged()
        if lifecycleToken == nil {
            lifecycleToken = AppLifecycle.shared.observe { (state: AppLifecycleState) in
                Task { @MainActor in
                    sleepUsageChanged(state)
                    if state == .inactive { WalkActivityController.shared.appLeaving() }
                    if state == .paused || state == .hidden { WalkActivityController.shared.appPaused() }
                    guard state == .resumed else { return }
                    WalkActivityController.shared.appResumed()
                    await syncSteps(health: health, force: false)
                    await syncSleep(force: false)
                }
            }
        }
    }

    @MainActor
    static func sleepUsageChanged(_ state: AppLifecycleState) {
        guard prefs.bool(forKey: HealthPrefs.sleep, or: false) else { return }
        if state == .resumed {
            SleepUsageLog.begin(prefs, now: Date())
        } else if state == .paused || state == .hidden || state == .detached {
            SleepUsageLog.end(prefs, now: Date())
        }
    }

    @MainActor
    static func sleepEnabledChanged() {
        let health: HealthKitBridge = bridge
        if prefs.bool(forKey: HealthPrefs.sleep, or: false) && health.available {
            health.observeSleep {
                await syncSleep(force: false)
            }
        } else {
            health.stopObservingSleep()
        }
    }

    @MainActor
    static func syncSleep(force: Bool) async {
        guard let api = Services.get(ApiService.self) else { return }
        _ = await SleepHealthSync.runIfDue(
            bridge: bridge, prefs: prefs, call: { method, suffix, body in try await api.sleepApi(method, suffix, body) },
            now: Date(), nights: 3, force: force)
    }

    @MainActor
    static func stepsEnabledChanged() {
        let health: HealthKitBridge = bridge
        if prefs.bool(forKey: HealthPrefs.steps, or: false) && health.available {
            health.observeSteps {
                await WalkActivityController.shared.catchUp()
                await syncSteps(health: health, force: false)
            }
            WalkActivityController.shared.start(health)
        } else {
            health.stopObservingSteps()
            WalkActivityController.shared.stop()
        }
    }

    @MainActor
    static func syncSteps(health: HealthBridge, force: Bool) async {
        guard let api = Services.get(ApiService.self) else { return }
        _ = await StepsHealthSync.runIfDue(
            health: health, prefs: prefs, call: { method, suffix, body in try await api.stepsApi(method, suffix, body) },
            now: Date(), days: 3, force: force)
    }
}
