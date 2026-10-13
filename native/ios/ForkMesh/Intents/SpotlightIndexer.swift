import AppIntents
import CoreSpotlight
import FMApi
import FMBase
import FMSurfaces
import Foundation
import UniformTypeIdentifiers

@MainActor
final class SpotlightIndexer {
    static let expiry: TimeInterval = 30 * 24 * 60 * 60

    private let recents: SurfaceRecentsStore?
    private var shortcutsRefresh: Task<Void, Never>?

    init(recents: SurfaceRecentsStore? = SurfaceRecentsStore.shared()) {
        self.recents = recents
    }

    func opened(_ route: Route, title: String, detail: String) {
        guard let recent = recents?.record(route, title: title, detail: detail) else { return }
        index(recent)
        if recent.ref.kind == .repo {
            refreshShortcutParameters()
        }
    }

    private func index(_ recent: SurfaceRecent) {
        let attributes = CSSearchableItemAttributeSet(contentType: .content)
        attributes.title = recent.title
        attributes.contentDescription = recent.detail
        attributes.keywords = [recent.ref.kind.label, "ForkMesh"]
        let item = CSSearchableItem(
            uniqueIdentifier: recent.ref.spotlightIdentifier, domainIdentifier: recent.ref.kind.rawValue,
            attributeSet: attributes)
        item.expirationDate = Date().addingTimeInterval(SpotlightIndexer.expiry)
        CSSearchableIndex.default().indexSearchableItems([item]) { (error: Error?) in
            if let error {
                Log.warning("surfaces", "Spotlight did not take an item: \(error)")
            }
        }
    }

    private func refreshShortcutParameters() {
        shortcutsRefresh?.cancel()
        shortcutsRefresh = Task {
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            if Task.isCancelled { return }
            ForkMeshShortcuts.updateAppShortcutParameters()
        }
    }

    func clear() {
        recents?.removeAll()
        CSSearchableIndex.default().deleteAllSearchableItems { (error: Error?) in
            if let error {
                Log.warning("surfaces", "Spotlight items were not removed: \(error)")
            }
        }
        ForkMeshShortcuts.updateAppShortcutParameters()
    }
}
