import FMBase
import Foundation
import Observation

public enum DirectCallKind: String, Equatable, Sendable {
    case audio
    case video
}

public enum DirectCallPhase: Equatable, Sendable {
    case ringing
    case connecting
    case live
}

public enum DirectCallConnection: String, Equatable, Sendable {
    case connected
    case disconnected
    case failed
    case stalled
}

public struct DirectCallEnding: Equatable, Sendable {
    public let title: String
    public let message: String

    public init(title: String, message: String) {
        self.title = title
        self.message = message
    }
}

public struct DirectCallInvite: Equatable, Sendable {
    public let conversation: String
    public let callId: String
    public let from: String
    public let partner: String
    public let media: DirectCallKind

    public init(conversation: String, callId: String, from: String, partner: String, media: DirectCallKind) {
        self.conversation = conversation
        self.callId = callId
        self.from = from
        self.partner = partner
        self.media = media
    }
}

@MainActor
public protocol DirectCallPeer: AnyObject {
    var onIceCandidate: (@MainActor (JSONObject) -> Void)? { get set }
    var onLive: (@MainActor () -> Void)? { get set }
    var onClosed: (@MainActor () -> Void)? { get set }
    var onConnection: (@MainActor (DirectCallConnection) -> Void)? { get set }
    func offer() async throws -> JSONObject
    func restartOffer() async throws -> JSONObject?
    func resumeMedia()
    func answer(offer: JSONObject) async throws -> JSONObject
    func acceptAnswer(_ answer: JSONObject) async throws
    func add(candidate: JSONObject) async
    func setMuted(_ muted: Bool)
    func setCameraOff(_ off: Bool)
    func setFace(_ id: String)
    func setFace(_ id: String, image: String)
    func setBackground(_ id: String, image: String)
    func setVoice(_ id: String)
    func observeVoice(_ handler: @escaping @MainActor (String) -> Void)
    func close()
}

extension DirectCallPeer {
    public var onConnection: (@MainActor (DirectCallConnection) -> Void)? {
        get { nil }
        set {}
    }
    public func restartOffer() async throws -> JSONObject? { nil }
    public func resumeMedia() {}
    public func setFace(_ id: String) {}
    public func setFace(_ id: String, image: String) { setFace(id) }
    public func setBackground(_ id: String, image: String) {}
    public func setVoice(_ id: String) {}
    public func observeVoice(_ handler: @escaping @MainActor (String) -> Void) {}
}

@MainActor
public protocol DirectCallPeerMaking: AnyObject {
    func makePeer(media: DirectCallKind, iceServers: [String]) async throws -> DirectCallPeer
}

@MainActor
@Observable
public final class DirectCallEngine {
    public static let iceServers = ["stun:stun.cloudflare.com:3478"]
    public static let ringTimeout: TimeInterval = 45
    public static let inviteResend: TimeInterval = 3
    public static let inviteStale: TimeInterval = 9
    public static let acceptResend: TimeInterval = 2
    public static let acceptResends = 8
    public static let offerResend: TimeInterval = 3
    public static let offerResends = 5
    public static let connectTimeout: TimeInterval = 30
    public static let dropGrace: TimeInterval = 2
    public static let restartEvery: TimeInterval = 6
    public static let reconnectBudget: TimeInterval = 30
    public static let connectFailed = DirectCallEnding(
        title: "Could not connect", message: "The call could not connect. Try again.")
    public static let dropped = DirectCallEnding(
        title: "Call lost", message: "The connection dropped and did not come back. Try calling again.")
    public static let ringTopic = "calls"
    public static let ringSound = "forkmesh-call-ring.wav"
    public static let ringSoundSeconds: Double = 27
    public static let frameTypes: Set<String> = [
        "call-invite", "call-accept", "call-decline", "call-offer", "call-answer", "call-ice", "call-bye",
    ]

    @ObservationIgnored let selfId: @MainActor () -> String
    @ObservationIgnored let accountName: @MainActor () -> String
    @ObservationIgnored let partnerFor: @MainActor (String) -> String
    @ObservationIgnored let sendSignal: @MainActor (String, String, JSONObject) async -> Bool
    @ObservationIgnored let sleep: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored public var peerFactory: DirectCallPeerMaking?
    @ObservationIgnored public var onMissed: (@MainActor (String, DirectCallKind) -> Void)?
    @ObservationIgnored public let look: FaceChangerLook
    @ObservationIgnored private var lookToken: UUID?

    public private(set) var conversation = ""
    public private(set) var callId = ""
    public private(set) var partner = ""
    public private(set) var media: DirectCallKind = .audio
    public private(set) var phase: DirectCallPhase?
    public private(set) var invite: DirectCallInvite?
    public private(set) var muted = false
    public private(set) var cameraOff = false
    public private(set) var voice = ""
    public private(set) var reconnecting = false
    public private(set) var liveSince: Date?
    public private(set) var ending: DirectCallEnding?

    @ObservationIgnored private var caller = false
    @ObservationIgnored private var peerId = ""
    @ObservationIgnored private var peer: DirectCallPeer?
    @ObservationIgnored private var pendingIce: [JSONObject] = []
    @ObservationIgnored private var remoteReady = false
    @ObservationIgnored private var ringTask: Task<Void, Never>?
    @ObservationIgnored private var inviteTask: Task<Void, Never>?
    @ObservationIgnored private var staleTask: Task<Void, Never>?
    @ObservationIgnored private var heldInvite = ""
    @ObservationIgnored private var acceptTask: Task<Void, Never>?
    @ObservationIgnored private var offerTask: Task<Void, Never>?
    @ObservationIgnored private var connectTask: Task<Void, Never>?
    @ObservationIgnored private var dropTask: Task<Void, Never>?
    @ObservationIgnored private var restartTask: Task<Void, Never>?
    @ObservationIgnored private var restartNow = false
    @ObservationIgnored private var restarting = false
    @ObservationIgnored private var dropRound = 0
    @ObservationIgnored private var offerReceived = false
    @ObservationIgnored private var answering = false
    @ObservationIgnored private var answeredOffer = ""
    @ObservationIgnored private var storedAnswer: JSONObject?
    @ObservationIgnored private var localOffer: JSONObject?
    @ObservationIgnored private var offerOutstanding = false
    @ObservationIgnored private var offerRound = 0
    @ObservationIgnored private var appliedAnswer = ""
    @ObservationIgnored private var answerApplied = false
    @ObservationIgnored private var holdingIce = false
    @ObservationIgnored private var heldIce: [JSONObject] = []

    public init(
        selfId: @escaping @MainActor () -> String,
        accountName: @escaping @MainActor () -> String,
        partnerFor: @escaping @MainActor (String) -> String,
        sendSignal: @escaping @MainActor (String, String, JSONObject) async -> Bool,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        },
        look: FaceChangerLook? = nil
    ) {
        self.selfId = selfId
        self.accountName = accountName
        self.partnerFor = partnerFor
        self.sendSignal = sendSignal
        self.sleep = sleep
        let look = look ?? FaceChangerLook()
        self.look = look
        lookToken = look.addListener { [weak self] part in self?.lookChanged(part) }
    }

    public func detach() {
        if let lookToken { look.removeListener(lookToken) }
        lookToken = nil
    }

    public var face: String { look.face }

    public var faceImage: String { look.faceImage }

    public var background: String { look.background }

    public var backgroundImage: String { look.backgroundImage }

    public var voiceChoice: String { look.voiceChoice }

    public var effectiveVoice: String { look.effectiveVoice }

    public var available: Bool { peerFactory != nil }

    public var active: Bool { phase != nil }

    public var stateLine: String {
        if reconnecting && phase != nil { return "Reconnecting…" }
        switch phase {
        case .ringing: return "Calling @\(partner)…"
        case .connecting: return "Connecting…"
        case .live: return "Connected · end-to-end encrypted"
        case nil: return ""
        }
    }

    public func partnerName(for conversation: String) -> String {
        conversation.isEmpty ? "" : partnerFor(conversation)
    }

    @discardableResult
    public func start(_ conversation: String, media: DirectCallKind) async -> Bool {
        guard !active, available, !conversation.isEmpty else { return false }
        let partner = partnerFor(conversation)
        guard !partner.isEmpty else { return false }
        self.conversation = conversation
        callId = UUID().uuidString.lowercased()
        self.partner = partner
        self.media = media
        phase = .ringing
        caller = true
        peerId = ""
        muted = false
        cameraOff = false
        ending = nil
        let id = callId
        _ = await sendSignal(conversation, "call-invite", ["call": .string(id), "media": .string(media.rawValue)])
        let pulse = sleep
        inviteTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await pulse(DirectCallEngine.inviteResend) } catch { return }
                guard let self, !Task.isCancelled, self.callId == id, self.phase == .ringing else { return }
                _ = await self.sendSignal(
                    self.conversation, "call-invite", ["call": .string(id), "media": .string(self.media.rawValue)])
            }
        }
        ringTask = Task { [weak self] in
            do { try await self?.sleep(DirectCallEngine.ringTimeout) } catch { return }
            guard let self, self.callId == id, self.phase == .ringing else { return }
            self.ringTask = nil
            await self.hangUp()
        }
        return true
    }

    public func holdInvite() {
        heldInvite = invite?.callId ?? ""
    }

    @discardableResult
    public func acceptInvite() async -> Bool {
        guard let pending = invite, !active, available else { return false }
        clearInvite()
        conversation = pending.conversation
        callId = pending.callId
        partner = pending.partner
        media = pending.media
        phase = .connecting
        caller = false
        peerId = pending.from
        muted = false
        cameraOff = false
        ending = nil
        armConnectTimeout()
        let id = pending.callId
        let fields: JSONObject = ["call": .string(pending.callId), "to": .string(pending.from)]
        _ = await sendSignal(pending.conversation, "call-accept", fields)
        let pulse = sleep
        acceptTask = Task { [weak self] in
            for _ in 0..<DirectCallEngine.acceptResends {
                do { try await pulse(DirectCallEngine.acceptResend) } catch { return }
                guard let self, !Task.isCancelled, self.callId == id, self.active, !self.caller, !self.offerReceived
                else { return }
                _ = await self.sendSignal(self.conversation, "call-accept", fields)
            }
        }
        return true
    }

    public func dismissEnding() {
        ending = nil
    }

    public func resumeMedia() {
        peer?.resumeMedia()
    }

    public func declineInvite() async {
        guard let pending = invite else { return }
        clearInvite()
        _ = await sendSignal(
            pending.conversation, "call-decline",
            ["call": .string(pending.callId), "to": .string(pending.from), "reason": "declined"])
    }

    public func hangUp() async {
        guard caller, phase == .ringing else {
            await end(notifyPeer: true, reason: "")
            return
        }
        let missedConversation = conversation
        let missedMedia = media
        await end(notifyPeer: true, reason: "missed")
        onMissed?(missedConversation, missedMedia)
    }

    public func toggleMute() {
        guard active else { return }
        muted.toggle()
        peer?.setMuted(muted)
    }

    public func toggleCamera() {
        guard active, media == .video else { return }
        cameraOff.toggle()
        peer?.setCameraOff(cameraOff)
    }

    public func setFace(_ id: String, image: String = "") {
        look.setFace(id, image: image)
    }

    public func setBackground(_ id: String, image: String = "") {
        look.setBackground(id, image: image)
    }

    public func setVoiceChoice(_ choice: String) {
        look.setVoiceChoice(choice)
    }

    private func lookChanged(_ part: FaceChangerPart) {
        switch part {
        case .face:
            if media == .video, let peer { applyFace(to: peer) }
            if voiceChoice == FaceChangerLook.voiceMatchFace { pushVoice() }
        case .background:
            if media == .video { peer?.setBackground(background, image: look.customBackgroundImage) }
        case .voice:
            pushVoice()
        }
    }

    private func applyFace(to made: DirectCallPeer) {
        if face == FaceChangerLook.customFace { made.setFace(face, image: faceImage) } else { made.setFace(face) }
    }

    private func pushVoice() {
        guard let peer else { return }
        voice = effectiveVoice
        peer.setVoice(voice)
    }

    public func handleFrame(_ plain: JSONObject, conversation scope: String) async {
        let type = plain["type"]?.text ?? ""
        guard DirectCallEngine.frameTypes.contains(type) else { return }
        let frameCall = plain["call"]?.text ?? ""
        let from = plain["senderId"]?.text ?? ""
        let me = selfId()
        if frameCall.isEmpty || from.isEmpty || from == me { return }
        let own = accountName()
        let fromOwnAccount = !own.isEmpty
            && (plain["sender"]?.text ?? "").trimmingCharacters(in: .whitespaces).lowercased() == own
        let to = plain["to"]?.text ?? ""
        if !to.isEmpty && to != me {
            if fromOwnAccount, invite?.callId == frameCall, type == "call-accept" || type == "call-decline" {
                clearInvite()
            }
            return
        }
        switch type {
        case "call-invite":
            if fromOwnAccount || (active && callId == frameCall) || !available { return }
            if active {
                _ = await sendSignal(
                    scope, "call-decline", ["call": .string(frameCall), "to": .string(from), "reason": "busy"])
                return
            }
            showInvite(plain, scope: scope, callId: frameCall, from: from)
        case "call-bye":
            if invite?.callId == frameCall { clearInvite() }
            if active && callId == frameCall { await end(notifyPeer: false, reason: "") }
        case "call-accept":
            guard active, callId == frameCall, caller else { return }
            if peerId.isEmpty {
                await connectAsCaller(to: from)
            } else if from == peerId, offerOutstanding, let localOffer {
                await sendOffer(localOffer)
            }
        case "call-decline":
            guard active, callId == frameCall, caller else { return }
            await end(notifyPeer: false, reason: "")
        case "call-offer":
            guard active, callId == frameCall, !caller, from == peerId,
                let sdp = plain["sdp"]?.objectValue
            else { return }
            offerReceived = true
            acceptTask?.cancel()
            acceptTask = nil
            let text = sdp["sdp"]?.text ?? ""
            if answering { return }
            if peer == nil {
                await connectAsCallee(offer: sdp, text: text)
            } else if text == answeredOffer, let storedAnswer {
                _ = await sendSignal(
                    conversation, "call-answer",
                    ["call": .string(callId), "to": .string(peerId), "sdp": .object(storedAnswer)])
            } else {
                await renegotiate(offer: sdp, text: text)
            }
        case "call-answer":
            guard active, callId == frameCall, caller, from == peerId, let peer,
                let sdp = plain["sdp"]?.objectValue
            else { return }
            let text = sdp["sdp"]?.text ?? ""
            guard offerOutstanding, !(answerApplied && text == appliedAnswer) else { return }
            offerOutstanding = false
            offerTask?.cancel()
            offerTask = nil
            let round = offerRound
            do {
                try await peer.acceptAnswer(sdp)
                appliedAnswer = text
                answerApplied = true
                remoteReady = true
                await flushPendingIce()
            } catch {
                if !answerApplied {
                    await end(notifyPeer: true, reason: "")
                } else if offerRound == round, self.peer === peer {
                    offerOutstanding = true
                }
            }
        case "call-ice":
            guard active, callId == frameCall, from == peerId, let candidate = plain["ice"]?.objectValue else { return }
            if remoteReady, !answering, !(caller && answerApplied && offerOutstanding), let peer {
                await peer.add(candidate: candidate)
            } else {
                pendingIce.append(candidate)
            }
        default:
            return
        }
    }

    private func showInvite(_ plain: JSONObject, scope: String, callId frameCall: String, from: String) {
        let media = DirectCallKind(rawValue: plain["media"]?.text ?? "") ?? .audio
        var partner = partnerFor(scope)
        if partner.isEmpty { partner = String((plain["sender"]?.text ?? "peer").prefix(63)) }
        invite = DirectCallInvite(conversation: scope, callId: frameCall, from: from, partner: partner, media: media)
        staleTask?.cancel()
        let id = frameCall
        staleTask = Task { [weak self] in
            do { try await self?.sleep(DirectCallEngine.inviteStale) } catch { return }
            guard let self, self.invite?.callId == id, self.heldInvite != id else { return }
            self.clearInvite()
        }
    }

    private func connectAsCaller(to deviceId: String) async {
        stopRinging()
        peerId = deviceId
        phase = .connecting
        armConnectTimeout()
        guard let factory = peerFactory else {
            await end(notifyPeer: true, reason: "")
            return
        }
        let id = callId
        do {
            let made = try await factory.makePeer(media: media, iceServers: DirectCallEngine.iceServers)
            guard callId == id, active else {
                made.close()
                return
            }
            attach(made)
            let sdp = try await made.offer()
            guard callId == id, peer === made else { return }
            await sendOffer(sdp)
        } catch {
            guard callId == id else { return }
            await end(notifyPeer: true, reason: "")
        }
    }

    private func sendOffer(_ sdp: JSONObject) async {
        let id = callId
        let target = peerId
        let scope = conversation
        if localOffer != sdp || !offerOutstanding {
            localOffer = sdp
            offerOutstanding = true
            offerRound += 1
            let round = offerRound
            let pulse = sleep
            offerTask?.cancel()
            offerTask = Task { [weak self] in
                for _ in 0..<DirectCallEngine.offerResends {
                    do { try await pulse(DirectCallEngine.offerResend) } catch { return }
                    guard let self, !Task.isCancelled, self.callId == id, self.offerOutstanding,
                        self.offerRound == round, let offer = self.localOffer
                    else { return }
                    _ = await self.sendSignal(
                        scope, "call-offer", ["call": .string(id), "to": .string(target), "sdp": .object(offer)])
                }
            }
        }
        _ = await sendSignal(scope, "call-offer", ["call": .string(id), "to": .string(target), "sdp": .object(sdp)])
    }

    private func connectAsCallee(offer: JSONObject, text: String) async {
        guard peer == nil, !answering, let factory = peerFactory else { return }
        answering = true
        let id = callId
        do {
            let made = try await factory.makePeer(media: media, iceServers: DirectCallEngine.iceServers)
            guard callId == id, active else {
                made.close()
                return
            }
            attach(made)
            let answer = try await made.answer(offer: offer)
            guard callId == id, peer === made else { return }
            answering = false
            answeredOffer = text
            storedAnswer = answer
            remoteReady = true
            await flushPendingIce()
            _ = await sendSignal(
                conversation, "call-answer",
                ["call": .string(callId), "to": .string(peerId), "sdp": .object(answer)])
        } catch {
            guard callId == id else { return }
            answering = false
            await end(notifyPeer: true, reason: "")
        }
    }

    private func renegotiate(offer: JSONObject, text: String) async {
        guard let current = peer, !answering else { return }
        answering = true
        holdingIce = true
        let id = callId
        let answer = try? await current.answer(offer: offer)
        guard callId == id, peer === current else { return }
        answering = false
        if let answer {
            answeredOffer = text
            storedAnswer = answer
            _ = await sendSignal(
                conversation, "call-answer", ["call": .string(callId), "to": .string(peerId), "sdp": .object(answer)])
        }
        releaseIce()
        await flushPendingIce()
    }

    private func releaseIce() {
        holdingIce = false
        let queued = heldIce
        heldIce = []
        guard !queued.isEmpty, !peerId.isEmpty else { return }
        let id = callId
        let target = peerId
        let scope = conversation
        Task { @MainActor in
            for candidate in queued {
                _ = await self.sendSignal(
                    scope, "call-ice", ["call": .string(id), "to": .string(target), "ice": .object(candidate)])
            }
        }
    }

    private func armConnectTimeout() {
        connectTask?.cancel()
        let id = callId
        let pulse = sleep
        connectTask = Task { [weak self] in
            do { try await pulse(DirectCallEngine.connectTimeout) } catch { return }
            guard let self, !Task.isCancelled, self.callId == id, self.active, self.phase != .live else { return }
            self.connectTask = nil
            self.ending = DirectCallEngine.connectFailed
            await self.end(notifyPeer: true, reason: "")
        }
    }

    private func connectionChanged(_ state: DirectCallConnection) {
        guard active, phase != .ringing else { return }
        switch state {
        case .connected:
            recovered()
        case .disconnected:
            lost(immediate: false)
        case .failed, .stalled:
            lost(immediate: true)
        }
    }

    private func recovered() {
        dropRound += 1
        reconnecting = false
        dropTask?.cancel()
        dropTask = nil
        restartTask?.cancel()
        restartTask = nil
        restartNow = false
    }

    private func lost(immediate: Bool) {
        if phase == .live {
            reconnecting = true
            armReconnectBudget()
        }
        guard caller else { return }
        if restartTask != nil && (restartNow || !immediate) { return }
        restartTask?.cancel()
        restartNow = immediate
        let round = dropRound
        let id = callId
        let pulse = sleep
        restartTask = Task { [weak self] in
            if !immediate {
                do { try await pulse(DirectCallEngine.dropGrace) } catch { return }
            }
            while true {
                guard let self, !Task.isCancelled, self.callId == id, self.dropRound == round else { return }
                await self.restart()
                do { try await pulse(DirectCallEngine.restartEvery) } catch { return }
            }
        }
    }

    private func armReconnectBudget() {
        guard dropTask == nil else { return }
        let round = dropRound
        let id = callId
        let pulse = sleep
        dropTask = Task { [weak self] in
            do { try await pulse(DirectCallEngine.reconnectBudget) } catch { return }
            guard let self, !Task.isCancelled, self.callId == id, self.dropRound == round, self.reconnecting else {
                return
            }
            self.dropTask = nil
            self.ending = DirectCallEngine.dropped
            await self.end(notifyPeer: true, reason: "")
        }
    }

    private func restart() async {
        guard caller, let current = peer, !restarting, !peerId.isEmpty else { return }
        restarting = true
        holdingIce = true
        let id = callId
        let made = (try? await current.restartOffer()) ?? nil
        restarting = false
        guard callId == id, peer === current else { return }
        if let made { await sendOffer(made) }
        releaseIce()
    }

    private func attach(_ made: DirectCallPeer) {
        peer = made
        if media == .video && !face.isEmpty { applyFace(to: made) }
        if media == .video && !background.isEmpty {
            made.setBackground(background, image: look.customBackgroundImage)
        }
        let id = callId
        voice = effectiveVoice
        if !voice.isEmpty { made.setVoice(voice) }
        made.observeVoice { [weak self] (applied: String) in
            guard let self, self.callId == id else { return }
            self.voice = applied
        }
        made.onIceCandidate = { [weak self] candidate in
            guard let self, self.callId == id, !self.peerId.isEmpty else { return }
            if self.holdingIce {
                self.heldIce.append(candidate)
                return
            }
            let target = self.peerId
            let scope = self.conversation
            Task { @MainActor in
                _ = await self.sendSignal(
                    scope, "call-ice", ["call": .string(id), "to": .string(target), "ice": .object(candidate)])
            }
        }
        made.onLive = { [weak self] in
            guard let self, self.callId == id else { return }
            self.phase = .live
            if self.liveSince == nil { self.liveSince = Date() }
            self.connectTask?.cancel()
            self.connectTask = nil
            self.recovered()
        }
        made.onConnection = { [weak self] (state: DirectCallConnection) in
            guard let self, self.callId == id else { return }
            self.connectionChanged(state)
        }
        made.onClosed = { [weak self] in
            guard let self, self.callId == id else { return }
            Task { @MainActor in await self.end(notifyPeer: false, reason: "") }
        }
    }

    private func flushPendingIce() async {
        guard let peer else { return }
        let queued = pendingIce
        pendingIce = []
        for candidate in queued { await peer.add(candidate: candidate) }
    }

    private func stopRinging() {
        inviteTask?.cancel()
        inviteTask = nil
        ringTask?.cancel()
        ringTask = nil
    }

    private func clearInvite() {
        staleTask?.cancel()
        staleTask = nil
        heldInvite = ""
        invite = nil
    }

    private func end(notifyPeer: Bool, reason: String) async {
        guard active else { return }
        stopRinging()
        for task in [acceptTask, offerTask, connectTask, dropTask, restartTask] { task?.cancel() }
        acceptTask = nil
        offerTask = nil
        connectTask = nil
        dropTask = nil
        restartTask = nil
        restartNow = false
        restarting = false
        dropRound += 1
        offerReceived = false
        answering = false
        answeredOffer = ""
        storedAnswer = nil
        localOffer = nil
        offerOutstanding = false
        appliedAnswer = ""
        answerApplied = false
        holdingIce = false
        heldIce = []
        reconnecting = false
        liveSince = nil
        let scope = conversation
        let id = callId
        conversation = ""
        callId = ""
        partner = ""
        phase = nil
        peerId = ""
        remoteReady = false
        pendingIce = []
        muted = false
        cameraOff = false
        voice = ""
        let closing = peer
        peer = nil
        closing?.onClosed = nil
        closing?.onConnection = nil
        closing?.close()
        if notifyPeer {
            var extra: JSONObject = ["call": .string(id)]
            if !reason.isEmpty { extra["reason"] = .string(reason) }
            _ = await sendSignal(scope, "call-bye", extra)
        }
    }
}
