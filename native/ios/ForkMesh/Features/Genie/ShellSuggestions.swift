import FMApi
import FMAssistant
import FMBase
import FMPackages
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class ShellSuggestions {
    static let shared: ShellSuggestions = ShellSuggestions()
    nonisolated static let onKey: String = "shell/suggestion_mode"
    nonisolated static let refreshEvery: Double = 10 * 60

    private(set) var on: Bool
    private(set) var contact: GenieOutreachPick?
    private(set) var contactImage: UIImage?

    @ObservationIgnored private let prefs: KeyValueStore?
    @ObservationIgnored private var passed: Set<String> = []
    @ObservationIgnored private var loading: Bool = false
    @ObservationIgnored private var again: Bool = false

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        self.prefs = prefs
        on = prefs?.bool(forKey: ShellSuggestions.onKey) ?? false
    }

    func setOn(_ value: Bool) {
        prefs?.set(value, forKey: ShellSuggestions.onKey)
        on = value
        if value {
            refreshNow()
        } else {
            contact = nil
            contactImage = nil
            if ShellAutoSend.shared.on { ShellAutoSend.shared.setOn(false) }
        }
    }

    func run() async {
        while !Task.isCancelled {
            await refresh()
            try? await Task.sleep(for: .seconds(ShellSuggestions.refreshEvery))
        }
    }

    func refreshNow() {
        Task { @MainActor [weak self] in
            await self?.refresh()
        }
    }

    func refresh() async {
        guard on else { return }
        guard !loading else {
            again = true
            return
        }
        loading = true
        defer { loading = false }
        repeat {
            again = false
            await refreshContact()
        } while again && on
    }

    func contactTapped() {
        guard let pick: GenieOutreachPick = contact, GenieOutreachCoach.shared.offer?.stage != .writing else { return }
        FmHaptics.tap()
        passed.insert(pick.contact.id)
        GenieOutreachCoach.shared.write(pick, image: contactImage)
        contact = nil
        contactImage = nil
        refreshNow()
    }

    func skipContact() {
        guard let pick: GenieOutreachPick = contact else { return }
        passed.insert(pick.contact.id)
        contact = nil
        contactImage = nil
        refreshNow()
    }

    private func refreshContact() async {
        guard let toolbox: ForkMeshToolbox = LocalAIFeature.toolbox() else {
            contact = nil
            contactImage = nil
            return
        }
        let auto: ShellAutoSend = ShellAutoSend.shared
        let skip: Set<String> = passed.union(GenieOutreachCoach.shared.log.draftedContacts).union(auto.skipped)
        let next: GenieOutreachPick? = auto.on
            ? await toolbox.outreachBest(skip: skip, only: .email, zohoOnly: true)
            : await toolbox.outreachBest(skip: skip)
        guard on else { return }
        if next?.contact.id == contact?.contact.id, next != nil {
            contact = next
            return
        }
        var picture: Data?
        if let next { picture = await GenieOutreachCoach.picture(for: next) }
        guard on else { return }
        contact = next
        contactImage = picture.flatMap { (data: Data) -> UIImage? in UIImage(data: data) }
    }
}
