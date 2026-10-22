import FMAccount
import FMApi
import FMBase
import FMPackages
import Foundation
import UIKit

@MainActor
final class RSSReaderSyncSource: AccountSyncSource {
    let key = "ios.rss"
    let kind = "subscriptions"
    let label = "News reader subscriptions"

    func snapshot() -> JSONValue? {
        guard let library = RSSReaderFeature.model().syncSnapshot(),
              let data = try? JSONEncoder().encode(library) else { return nil }
        return try? JSON.parse(data)
    }

    func restore(_ value: JSONValue) -> Int {
        guard let library = try? JSONDecoder().decode(RSSReaderLibrary.self, from: JSON.encodeData(value)) else { return 0 }
        return RSSReaderFeature.model().restoreSync(library)
    }
}

@MainActor
enum AccountSyncSetup {
    static func install() {
        guard Services.get(AccountSyncService.self) == nil,
              let api = Services.get(ApiService.self),
              let prefs = Services.get(KeyValueStore.self) else { return }
        let auth = Services.get(AuthService.self)
        let service = AccountSyncService(
            client: api, prefs: prefs,
            sources: [PreferenceSyncSource.home(prefs), RSSReaderSyncSource(), PreferenceSyncSource.settings(prefs)],
            device: UIDevice.current.model,
            account: { auth?.session?.nodeName ?? "" })
        Services.put(service)
        AppLifecycle.shared.observe { (state: AppLifecycleState) in
            guard state == .paused || state == .resumed else { return }
            guard service.enabled else { return }
            let task = UIApplication.shared.beginBackgroundTask(withName: "Save to ForkMesh", expirationHandler: nil)
            Task { @MainActor in
                await service.saveIfChanged()
                if task != .invalid { UIApplication.shared.endBackgroundTask(task) }
            }
        }
    }
}
