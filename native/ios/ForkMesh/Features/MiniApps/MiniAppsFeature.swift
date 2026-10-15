import BackgroundTasks
import FMAccount
import FMBase
import FMControl
import FMMiniApps
import Foundation

@MainActor
enum MiniAppsFeature {
    static var client: MiniAppClient? {
        if let existing = Services.get(MiniAppClient.self) { return existing }
        guard let settings = Services.get(SettingsService.self) else { return nil }
        let client = MiniAppClient(store: settings.prefs, link: { DesktopRemoteFeature.hub?.currentLink })
        Services.put(client)
        _ = DesktopRemoteFeature.hub?.addLinkListener { [weak client] in
            guard let client, client.reach.canCall, !client.outbox.pending.isEmpty else { return }
            Task { _ = await client.flushOutbox() }
        }
        _ = AppLifecycle.shared.observe { state in
            if state == .paused { scheduleBackgroundRefresh() }
        }
        return client
    }

    static func registerBackgroundRefresh() {
        BGTaskScheduler.shared.register(forTaskWithIdentifier: MiniAppBackgroundPlan.refreshTaskIdentifier, using: nil) { task in
            Task { @MainActor in
                await refresh(task: task as? BGAppRefreshTask)
            }
        }
    }

    static func scheduleBackgroundRefresh() {
        guard let client else { return }
        let cards = (client.cachedApps).compactMap { client.cachedCard(for: $0)?.card }
        guard let next = MiniAppBackgroundPlan.nextRefresh(cards: cards, pendingIntents: client.outbox.pending.count, now: Date()) else {
            BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: MiniAppBackgroundPlan.refreshTaskIdentifier)
            return
        }
        let request = BGAppRefreshTaskRequest(identifier: MiniAppBackgroundPlan.refreshTaskIdentifier)
        request.earliestBeginDate = next
        try? BGTaskScheduler.shared.submit(request)
    }

    private static func refresh(task: BGAppRefreshTask?) async {
        scheduleBackgroundRefresh()
        guard let client else {
            task?.setTaskCompleted(success: false)
            return
        }
        let work = Task { @MainActor in
            _ = await client.flushOutbox()
            guard client.reach.canCall, let apps = try? await client.catalog() else { return false }
            client.cachedApps = apps
            for app in apps where app.hasCard {
                if Task.isCancelled { return false }
                _ = try? await client.card(for: app)
            }
            return true
        }
        task?.expirationHandler = { work.cancel() }
        let ok = await work.value
        task?.setTaskCompleted(success: ok)
    }
}

extension MiniAppClient {
    private static let appsKey = "miniapps/catalog"

    var cachedApps: [MiniAppDescriptor] {
        get {
            guard let settings = Services.get(SettingsService.self),
                  let text = settings.prefs.string(forKey: Self.appsKey) else { return [] }
            return MiniAppDescriptor.catalog(JSON.parseObject(text))
        }
        set {
            guard let settings = Services.get(SettingsService.self) else { return }
            let rows: [JSONValue] = newValue.map {
                .object(["id": .string($0.id), "name": .string($0.name), "version": .string($0.version),
                         "summary": .string($0.summary), "methods": JSONValue($0.methods), "events": JSONValue($0.events)])
            }
            settings.prefs.set(JSON.encode(["plugins": .array(rows)]), forKey: Self.appsKey)
        }
    }
}
