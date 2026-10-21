import CallKit
import FMApi
import FMBase
import FMChat
import Foundation
import Observation
import PushKit
import StoreKit

@MainActor
final class DirectCallKit: NSObject {
    static let shared: DirectCallKit = DirectCallKit()

    private lazy var provider: CXProvider = makeProvider()
    private let controller: CXCallController = CXCallController()
    private let ledger: DirectCallKitLedger = DirectCallKitLedger()
    private var registry: PKPushRegistry?
    private weak var engine: DirectCallEngine?
    private weak var direct: DirectMessageService?
    private var waitingOpens: [String] = []
    private var voipToken: String = ""
    private var tokenShared: Bool = false
    private var allowed: Bool = false
    private var storefrontWatch: Task<Void, Never>?
    var wake: (@MainActor () -> Void)?

    private static let storefrontKey: String = "callkit.storefront"

    override private init() {
        super.init()
    }

    private func makeProvider() -> CXProvider {
        let configuration: CXProviderConfiguration = CXProviderConfiguration()
        configuration.supportsVideo = true
        configuration.maximumCallGroups = 1
        configuration.maximumCallsPerCallGroup = 1
        configuration.supportedHandleTypes = [.generic]
        configuration.includesCallsInRecents = false
        let made: CXProvider = CXProvider(configuration: configuration)
        made.setDelegate(self, queue: .main)
        return made
    }

    func start() {
        setAllowed(Self.regionAllows(storefront: UserDefaults.standard.string(forKey: Self.storefrontKey)))
        guard storefrontWatch == nil else { return }
        storefrontWatch = Task { @MainActor [weak self] in
            if let current = await Storefront.current { self?.storefrontChanged(current.countryCode) }
            for await update in Storefront.updates { self?.storefrontChanged(update.countryCode) }
        }
    }

    private func storefrontChanged(_ countryCode: String) {
        UserDefaults.standard.set(countryCode, forKey: Self.storefrontKey)
        setAllowed(Self.regionAllows(storefront: countryCode))
    }

    private static func regionAllows(storefront: String?) -> Bool {
        DirectCallKitRegion.allowed(storefront: storefront, region: Locale.current.region?.identifier)
    }

    private func setAllowed(_ value: Bool) {
        let changed: Bool = value != allowed || (value && registry == nil)
        allowed = value
        guard changed else { return }
        if value {
            let made: PKPushRegistry = PKPushRegistry(queue: .main)
            made.delegate = self
            made.desiredPushTypes = [.voIP]
            registry = made
        } else {
            registry?.desiredPushTypes = []
            registry = nil
            voipToken = ""
            apply(ledger.endAll())
        }
        tokenShared = false
        shareToken()
        CallRinger.shared.recheck()
    }

    func attach(_ engine: DirectCallEngine, direct: DirectMessageService) {
        self.engine = engine
        self.direct = direct
        observe(engine)
        let opens: [String] = waitingOpens
        waitingOpens.removeAll()
        for conversation in opens {
            Task { await direct.openRinging(conversation) }
        }
        tokenShared = false
        shareToken()
        sync()
    }

    func ringing(_ callId: String) -> Bool {
        allowed && ledger.ringing(callId)
    }

    private func observe(_ engine: DirectCallEngine) {
        withObservationTracking {
            _ = engine.invite
            _ = engine.phase
            _ = engine.callId
        } onChange: { [weak self, weak engine] in
            Task { @MainActor [weak self, weak engine] in
                guard let self, let engine, self.engine === engine else { return }
                self.sync()
                self.observe(engine)
            }
        }
    }

    private func sync() {
        guard allowed, let engine else { return }
        apply(
            ledger.engine(
                invite: engine.active ? nil : engine.invite, activeCallId: engine.active ? engine.callId : ""))
    }

    private func shareToken() {
        guard !tokenShared, let push = Services.get(PushNotificationService.self) else { return }
        tokenShared = true
        let token: String = voipToken
        Task { await push.didReceiveVoipToken(token) }
    }

    private func received(_ fields: [String: String], completion: @escaping () -> Void) {
        if engine == nil { wake?() }
        guard allowed, let push = DirectCallRingPush(fields: fields) else {
            let id: String = UUID().uuidString.lowercased()
            apply([.report(id: id, caller: "", video: false), .end(id: id, reason: .failed)], reported: completion)
            return
        }
        apply(ledger.push(push), reported: completion)
    }

    private func apply(_ effects: [DirectCallKitEffect], reported: (() -> Void)? = nil) {
        var rest: [DirectCallKitEffect] = effects
        while !rest.isEmpty {
            let effect: DirectCallKitEffect = rest.removeFirst()
            switch effect {
            case let .report(id, caller, video):
                let after: [DirectCallKitEffect] = rest
                report(id, caller: caller, video: video) { [weak self] in
                    reported?()
                    CallRinger.shared.recheck()
                    self?.apply(after)
                }
                return
            case let .update(id, caller, video):
                guard let uuid: UUID = UUID(uuidString: id) else { continue }
                provider.reportCall(with: uuid, updated: Self.update(caller: caller, video: video))
            case let .end(id, reason):
                guard let uuid: UUID = UUID(uuidString: id) else { continue }
                provider.reportCall(with: uuid, endedAt: nil, reason: Self.reason(reason))
            case let .open(conversation):
                if let direct {
                    Task { await direct.openRinging(conversation) }
                } else {
                    waitingOpens.append(conversation)
                }
            case let .wait(id):
                Task { @MainActor [weak self] in
                    try? await Task.sleep(nanoseconds: UInt64(DirectCallKitLedger.inviteWait * 1_000_000_000))
                    guard let self else { return }
                    self.apply(self.ledger.timedOut(id))
                    CallRinger.shared.recheck()
                }
            case let .accept(callId):
                accept(callId)
            case let .decline(callId):
                Task { @MainActor [weak self] in
                    guard let engine = self?.engine, engine.invite?.callId.lowercased() == callId else { return }
                    await engine.declineInvite()
                }
            case .hangUp:
                Task { @MainActor [weak self] in await self?.engine?.hangUp() }
            case let .answerInApp(id):
                guard let uuid: UUID = UUID(uuidString: id) else { continue }
                controller.request(CXTransaction(action: CXAnswerCallAction(call: uuid))) { _ in }
            }
        }
        reported?()
    }

    private func report(_ id: String, caller: String, video: Bool, done: @escaping () -> Void) {
        guard let uuid: UUID = UUID(uuidString: id) else {
            ledger.reportFailed(id)
            done()
            return
        }
        provider.reportNewIncomingCall(with: uuid, update: Self.update(caller: caller, video: video)) { error in
            Task { @MainActor [weak self] in
                if let error, let self, Self.fallsBack(error) { self.ledger.reportFailed(id) }
                done()
            }
        }
    }

    private func accept(_ callId: String) {
        Task { @MainActor [weak self] in
            guard let engine = self?.engine, let invite = engine.invite, invite.callId.lowercased() == callId else {
                return
            }
            CallRinger.shared.hush()
            guard await WebCallPeerFactory.requestAccess(for: invite.media) else {
                await engine.declineInvite()
                return
            }
            await engine.acceptInvite()
        }
    }

    private static func fallsBack(_ error: Error) -> Bool {
        guard let code = (error as? CXErrorCodeIncomingCallError)?.code else { return true }
        switch code {
        case .callUUIDAlreadyExists, .filteredByDoNotDisturb, .filteredByBlockList: return false
        default: return true
        }
    }

    private static func update(caller: String, video: Bool) -> CXCallUpdate {
        let update: CXCallUpdate = CXCallUpdate()
        let name: String = caller.isEmpty ? "ForkMesh call" : "@\(caller)"
        update.remoteHandle = CXHandle(type: .generic, value: name)
        update.localizedCallerName = name
        update.hasVideo = video
        update.supportsHolding = false
        update.supportsGrouping = false
        update.supportsUngrouping = false
        update.supportsDTMF = false
        return update
    }

    private static func reason(_ reason: DirectCallKitEndReason) -> CXCallEndedReason {
        switch reason {
        case .remoteEnded: return .remoteEnded
        case .unanswered: return .unanswered
        case .failed: return .failed
        }
    }

    nonisolated private static func fields(_ payload: [AnyHashable: Any]) -> [String: String] {
        var fields: [String: String] = [:]
        for (key, value) in payload {
            guard let key = key as? String else { continue }
            if let text = value as? String {
                fields[key] = text
            } else if let number = value as? NSNumber {
                fields[key] = number.stringValue
            }
        }
        return fields
    }
}

extension DirectCallKit: CXProviderDelegate {
    nonisolated func providerDidReset(_ provider: CXProvider) {
        MainActor.assumeIsolated {
            apply(ledger.reset())
            CallRinger.shared.recheck()
        }
    }

    nonisolated func provider(_ provider: CXProvider, perform action: CXAnswerCallAction) {
        let id: String = action.callUUID.uuidString.lowercased()
        MainActor.assumeIsolated {
            apply(ledger.answered(id))
            CallRinger.shared.recheck()
        }
        action.fulfill()
    }

    nonisolated func provider(_ provider: CXProvider, perform action: CXEndCallAction) {
        let id: String = action.callUUID.uuidString.lowercased()
        MainActor.assumeIsolated {
            apply(ledger.endRequested(id))
            CallRinger.shared.recheck()
        }
        action.fulfill()
    }

    nonisolated func provider(_ provider: CXProvider, perform action: CXSetMutedCallAction) {
        let muted: Bool = action.isMuted
        MainActor.assumeIsolated {
            if let engine, engine.active, engine.muted != muted { engine.toggleMute() }
        }
        action.fulfill()
    }
}

extension DirectCallKit: PKPushRegistryDelegate {
    nonisolated func pushRegistry(
        _ registry: PKPushRegistry, didUpdate pushCredentials: PKPushCredentials, for type: PKPushType
    ) {
        guard type == .voIP else { return }
        let token: String = pushCredentials.token.map { String(format: "%02x", $0) }.joined()
        MainActor.assumeIsolated {
            voipToken = token
            tokenShared = false
            shareToken()
        }
    }

    nonisolated func pushRegistry(_ registry: PKPushRegistry, didInvalidatePushTokenFor type: PKPushType) {
        guard type == .voIP else { return }
        MainActor.assumeIsolated {
            voipToken = ""
            tokenShared = false
            shareToken()
        }
    }

    nonisolated func pushRegistry(
        _ registry: PKPushRegistry, didReceiveIncomingPushWith payload: PKPushPayload, for type: PKPushType,
        completion: @escaping () -> Void
    ) {
        guard type == .voIP else {
            completion()
            return
        }
        let fields: [String: String] = Self.fields(payload.dictionaryPayload)
        MainActor.assumeIsolated { received(fields, completion: completion) }
    }
}
