import FMAssistant
import FMBase
import Foundation
import Observation
import SwiftUI
import UIKit

enum GenieOutreachStage: Equatable {
    case offered
    case writing
}

struct GenieOutreachOffer: Identifiable, Equatable {
    let id: Int
    let pick: GenieOutreachPick
    var image: UIImage?
    var stage: GenieOutreachStage
}

struct GenieOutreachDeckMissing: Error, CustomStringConvertible {
    let reason: String

    var description: String { reason }
}

@MainActor
@Observable
final class GenieOutreachCoach {
    static let shared: GenieOutreachCoach = GenieOutreachCoach()
    nonisolated static let checkEvery: Double = 60
    nonisolated static let showSeconds: Double = 30
    nonisolated static let imageBytes: Int = 2_000_000

    private(set) var offer: GenieOutreachOffer?
    private(set) var off: Bool
    var stageHidden: Bool = false

    @ObservationIgnored let log: GenieOutreachLog
    @ObservationIgnored private var activeSince: Date = Date()
    @ObservationIgnored private var active: Bool = true
    @ObservationIgnored private var count: Int = 0
    @ObservationIgnored private var looking: Bool = false
    @ObservationIgnored private var fade: Task<Void, Never>?

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        log = GenieOutreachLog(store: prefs)
        off = log.off
    }

    private var quiet: Bool {
        stageHidden || !active || !GenieHub.shared.calm
    }

    func scene(active now: Bool) {
        if now && !active { activeSince = Date() }
        active = now
    }

    func run() async {
        while !Task.isCancelled {
            try? await Task.sleep(for: .seconds(GenieOutreachCoach.checkEvery))
            if Task.isCancelled { return }
            await consider(force: false)
        }
    }

    func setOff(_ value: Bool) {
        log.setOff(value)
        off = value
        if value { dismiss() }
    }

    func suggestNow() {
        Task { @MainActor [weak self] in
            await self?.consider(force: true)
        }
    }

    func consider(force: Bool, now: Date = Date()) async {
        guard offer == nil, !looking, !off else { return }
        guard force || log.due(now: now, activeSince: activeSince), !quiet else { return }
        guard let toolbox: ForkMeshToolbox = LocalAIFeature.toolbox() else { return }
        looking = true
        defer { looking = false }
        guard let pick: GenieOutreachPick = await toolbox.outreachPick(skip: log.skip(now: now)) else {
            log.checked(at: now)
            if force { GenieHub.shared.show(line: "Nobody new to contact in your CRM right now.") }
            return
        }
        let picture: Data? = await GenieOutreachCoach.picture(for: pick)
        let image: UIImage? = picture.flatMap { (data: Data) -> UIImage? in UIImage(data: data) }
        guard offer == nil, !off, !quiet else { return }
        log.showed(pick.contact.id, at: now)
        count += 1
        GenieCrawler.shared.halt()
        GenieSounds.shared.play(.spotted)
        GenieCues.shared.play(.surprise)
        FmHaptics.impact(.light)
        withAnimation(.spring(response: 0.45, dampingFraction: 0.66)) {
            offer = GenieOutreachOffer(id: count, pick: pick, image: image, stage: .offered)
        }
        UIAccessibility.post(notification: .announcement, argument: "\(pick.caption) Tap their picture and the Genie writes it.")
        let id: Int = count
        fade?.cancel()
        fade = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(GenieOutreachCoach.showSeconds))
            guard !Task.isCancelled, let self = self, self.offer?.id == id, self.offer?.stage == .offered else { return }
            self.dismiss()
        }
    }

    func dismiss() {
        fade?.cancel()
        guard offer != nil else { return }
        withAnimation(.easeOut(duration: 0.25)) { offer = nil }
    }

    func write(_ pick: GenieOutreachPick, image: UIImage?) {
        guard offer?.stage != .writing else { return }
        fade?.cancel()
        count += 1
        offer = GenieOutreachOffer(id: count, pick: pick, image: image, stage: .offered)
        write()
    }

    func write() {
        guard var current: GenieOutreachOffer = offer, current.stage == .offered else { return }
        fade?.cancel()
        FmHaptics.impact(.medium)
        GenieSounds.shared.play(.whoosh)
        GenieCues.shared.play(.eureka)
        current.stage = .writing
        withAnimation(.easeOut(duration: 0.2)) { offer = current }
        let id: Int = current.id
        let pick: GenieOutreachPick = current.pick
        Task { @MainActor [weak self] in
            var deck: String = ""
            if pick.needsDeck {
                do {
                    guard let toolbox: ForkMeshToolbox = LocalAIFeature.toolbox() else {
                        throw GenieOutreachDeckMissing(reason: "The CRM is not available.")
                    }
                    deck = try await toolbox.outreachDeck(pick)
                } catch {
                    guard let self = self, self.offer?.id == id else { return }
                    self.dismiss()
                    GenieHub.shared.show(line: "No email to \(pick.contact.displayName): investor emails carry their own deck link, and I could not make one. \(error)")
                    return
                }
            }
            guard let self = self, self.offer?.id == id else { return }
            self.log.drafted(pick.contact.id)
            let draft: GenieOutreachDraft = await GenieOutreach.write(pick, writers: GenieFeature.outreachWriters(), deck: deck)
            guard self.offer?.id == id else { return }
            self.dismiss()
            self.open(draft)
        }
    }

    private func open(_ draft: GenieOutreachDraft) {
        GenieCues.shared.play(.cheer)
        switch draft.channel {
        case .email:
            guard let url: URL = draft.zohoURL else {
                share(draft)
                return
            }
            UIApplication.shared.open(url) { [weak self] (opened: Bool) in
                guard opened else {
                    self?.share(draft)
                    return
                }
                GenieHub.shared.show(line: GenieOutreachCoach.openedLine(draft))
            }
        case .mastodon:
            UIPasteboard.general.string = draft.body
            share(draft)
            GenieHub.shared.show(line: "Toot copied. Pick Mastodon to post it.")
        }
    }

    nonisolated static func openedLine(_ draft: GenieOutreachDraft) -> String {
        var line: String = draft.byModel ? "Opened in Zoho Mail, written by \(draft.writer)." : "Opened in Zoho Mail from a template."
        if !draft.deck.isEmpty { line += " The deck link is theirs alone, so the CRM shows when they open it and what they read." }
        return line + " Check it before you send."
    }

    private func share(_ draft: GenieOutreachDraft) {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        guard var top: UIViewController = scene?.windows.first(where: { $0.isKeyWindow })?.rootViewController else { return }
        while let next: UIViewController = top.presentedViewController { top = next }
        let text: String = draft.channel == .email ? "Subject: \(draft.subject)\n\n\(draft.body)" : draft.body
        let controller: UIActivityViewController = UIActivityViewController(activityItems: [text], applicationActivities: nil)
        if let popover: UIPopoverPresentationController = controller.popoverPresentationController {
            let place: CGPoint = GenieCrawler.shared.point(at: Date())
            popover.sourceView = top.view
            popover.sourceRect = CGRect(x: place.x, y: place.y, width: 1, height: 1)
        }
        top.present(controller, animated: true)
    }

    nonisolated static func picture(for pick: GenieOutreachPick) async -> Data? {
        var urls: [URL] = pick.images
        if let lookup: URL = pick.handle?.lookupURL, let account: Data = await fetch(lookup),
           let json: [String: Any] = (try? JSONSerialization.jsonObject(with: account)) as? [String: Any],
           let avatar: String = json["avatar_static"] as? String ?? json["avatar"] as? String,
           let url: URL = URL(string: avatar), url.scheme == "https", !avatar.contains("/missing.png") {
            urls.insert(url, at: 0)
        }
        for url in urls.prefix(4) {
            if let data: Data = await fetch(url), UIImage(data: data) != nil { return data }
        }
        return nil
    }

    nonisolated private static func fetch(_ url: URL) async -> Data? {
        var request: URLRequest = URLRequest(url: url, timeoutInterval: 8)
        request.setValue("ForkMesh-iOS", forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              let http: HTTPURLResponse = response as? HTTPURLResponse, http.statusCode == 200,
              data.count <= GenieOutreachCoach.imageBytes
        else { return nil }
        return data
    }
}

struct GenieOutreachCard: View {
    let offer: GenieOutreachOffer
    let onWrite: () -> Void
    let onClose: () -> Void

    private var contact: String { offer.pick.contact.displayName }

    private var symbol: String { offer.pick.channel == .email ? "envelope.fill" : "bubble.left.and.text.bubble.right.fill" }

    var body: some View {
        VStack(spacing: FmSpace.x1) {
            Button(action: onWrite) {
                portrait
                    .overlay(alignment: .bottomTrailing) { badge }
            }
            .buttonStyle(.plain)
            .disabled(offer.stage == .writing)
            .accessibilityLabel(Text(offer.stage == .writing ? "Writing to \(contact)" : offer.pick.caption))
            .accessibilityHint(Text("Writes the message and opens it in \(offer.pick.channel == .email ? "Mail" : "Mastodon")"))
            .accessibilityIdentifier("genie-outreach-write")
            HStack(spacing: FmSpace.x1) {
                if offer.stage == .writing {
                    ProgressView()
                        .controlSize(.mini)
                        .tint(GenieInk.glow)
                }
                Text(offer.stage == .writing ? "Writing to \(offer.pick.firstName.isEmpty ? contact : offer.pick.firstName)…" : offer.pick.caption)
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Color.white)
                    .lineLimit(1)
                if offer.stage == .offered {
                    Button(action: onClose) {
                        Image(systemName: "xmark")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundStyle(Color.white.opacity(0.8))
                            .frame(width: 18, height: 18)
                            .background(Circle().fill(Color.white.opacity(0.14)))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(Text("Not now"))
                    .accessibilityIdentifier("genie-outreach-close")
                }
            }
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, 4)
            .frame(maxWidth: 220)
            .background(Capsule().fill(LinearGradient(colors: [GenieSheet.night, GenieSheet.dusk], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
            .overlay(Capsule().strokeBorder(GenieInk.glow.opacity(0.5), lineWidth: 1))
        }
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("genie-outreach")
    }

    @ViewBuilder private var portrait: some View {
        Group {
            if let image: UIImage = offer.image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ZStack {
                    LinearGradient(colors: [GenieInk.violet, GenieSheet.dusk], startPoint: UnitPoint.topLeading, endPoint: UnitPoint.bottomTrailing)
                    Text(offer.pick.contact.initials)
                        .font(.title3.weight(.bold))
                        .foregroundStyle(Color.white)
                }
            }
        }
        .frame(width: 60, height: 60)
        .clipShape(Circle())
        .overlay(Circle().strokeBorder(GenieInk.glow, lineWidth: 2))
        .shadow(color: GenieInk.glow.opacity(0.55), radius: 10)
        .opacity(offer.stage == .writing ? 0.7 : 1)
    }

    private var badge: some View {
        Image(systemName: symbol)
            .font(.system(size: 10, weight: .bold))
            .foregroundStyle(GenieSheet.night)
            .frame(width: 22, height: 22)
            .background(Circle().fill(GenieInk.glow))
            .overlay(Circle().strokeBorder(GenieSheet.night, lineWidth: 1.5))
            .offset(x: 3, y: 3)
    }
}
