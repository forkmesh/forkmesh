// View models of two small account surfaces: the sponsored ad card (the one
// ForkMesh Ads slot) and the Settings screen's push notifications card (where
// the APNs chain stands, and a test alert).

import FMAccount
import FMBase
import Foundation
import Observation

/// The single ForkMesh Ads slot. No ad, a failed request, a switched-off
/// preference or a "Hide" still in force all show nothing: no placeholder,
/// no error. One request per appearance; the network call is cached and
/// never throws, so there is no retry path.
@MainActor
@Observable
public final class SponsoredAdModel {
    public private(set) var ad: SponsoredAd?
    /// Without a SettingsService the dismissal lasts for this card only.
    public private(set) var hiddenLocally = false

    @ObservationIgnored private let settings: SettingsService?
    @ObservationIgnored private let fetch: () async -> SponsoredAd?
    @ObservationIgnored private let clicked: (String) async -> Void
    @ObservationIgnored private var requested = false

    public init(
        settings: SettingsService?,
        fetch: @escaping () async -> SponsoredAd?,
        clicked: @escaping (String) async -> Void
    ) {
        self.settings = settings
        self.fetch = fetch
        self.clicked = clicked
    }

    public convenience init(settings: SettingsService?, api: ApiService) {
        self.init(
            settings: settings,
            fetch: { await api.sponsoredAd() },
            clicked: { id in await api.sponsoredAdClicked(id) })
    }

    /// While sponsored messages are off or hidden the network is not asked.
    public var allowed: Bool {
        guard let settings else { return true }
        return settings.showSponsoredMessages && !settings.sponsoredMessagesHidden
    }

    /// The ad to draw, or nil for an empty slot.
    public var visibleAd: SponsoredAd? {
        guard let ad, !hiddenLocally, allowed, ad.uri != nil else { return nil }
        return ad
    }

    /// Run on appearance and whenever `allowed` turns true.
    public func load() async {
        if requested || !allowed { return }
        requested = true
        ad = await fetch()
    }

    /// The CTA: the caller opens the returned link; the click is recorded
    /// best-effort.
    public func follow(_ ad: SponsoredAd) -> URL? {
        let id = ad.id
        let record = clicked
        Task { await record(id) }
        return ad.uri
    }

    /// "Hide": seven days with a SettingsService, this card only without.
    public func hide() {
        settings?.hideSponsoredMessages()
        hiddenLocally = true
    }
}

/// `_PushNotificationsCard`.
@MainActor
@Observable
public final class PushCardModel {
    public let push: PushNotificationService
    public private(set) var sending = false

    public init(push: PushNotificationService) {
        self.push = push
    }

    public var permissionLabel: String { pushPermissionLabel(push.permission) }
    public var tokenLabel: String {
        pushTokenLabel(
            token: push.token, permission: push.permission, environment: push.environment, registered: push.registered)
    }
    public var relayLabel: String { pushRelayLabel(push.relayConfigured) }
    public var canEnable: Bool { pushCanEnable(token: push.token, permission: push.permission) }
    public var canSendTest: Bool { !sending && !push.token.isEmpty }
    public var sendLabel: String { pushSendLabel(sending: sending) }
    public var denied: Bool { push.permission == "denied" }

    public func enable() async {
        await push.enable()
    }

    /// The result's sentence, for the card's inline notice.
    public func sendTest() async -> String {
        sending = true
        let result = await push.sendTest()
        sending = false
        return result.summary
    }
}
