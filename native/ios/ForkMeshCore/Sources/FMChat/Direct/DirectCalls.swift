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

public struct DirectCallFace: Equatable, Sendable, Identifiable {
    public let id: String
    public let label: String

    public init(id: String, label: String) {
        self.id = id
        self.label = label
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
    func offer() async throws -> JSONObject
    func answer(offer: JSONObject) async throws -> JSONObject
    func acceptAnswer(_ answer: JSONObject) async throws
    func add(candidate: JSONObject) async
    func setMuted(_ muted: Bool)
    func setCameraOff(_ off: Bool)
    func setFace(_ id: String)
    func setBackground(_ id: String, image: String)
    func setVoice(_ id: String)
    func observeVoice(_ handler: @escaping @MainActor (String) -> Void)
    func close()
}

extension DirectCallPeer {
    public func setFace(_ id: String) {}
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
    public static let faceKey = "chat.callFace"
    public static let faces: [DirectCallFace] = [
        DirectCallFace(id: "maya", label: "Maya"),
        DirectCallFace(id: "theo", label: "Theo"),
        DirectCallFace(id: "kofi", label: "Kofi"),
        DirectCallFace(id: "ingrid", label: "Ingrid"),
        DirectCallFace(id: "ren", label: "Ren"),
    ]
    public static let backgroundKey = "chat.callBackground"
    public static let customBackground = "custom"
    public static let backgrounds: [DirectCallFace] = [
        DirectCallFace(id: "", label: "No background"),
        DirectCallFace(id: "blur", label: "Blur"),
        DirectCallFace(id: "blur-strong", label: "Strong blur"),
        DirectCallFace(id: "world", label: "ForkMesh World"),
        DirectCallFace(id: "office", label: "ForkMesh office"),
        DirectCallFace(id: "meeting", label: "Meeting room"),
        DirectCallFace(id: "library", label: "Home library"),
        DirectCallFace(id: "skyline", label: "City window"),
        DirectCallFace(id: "custom", label: "Your photo"),
    ]
    public static let voiceKey = "chat.callVoice"
    public static let voiceMatchFace = "face"
    public static let voiceOff = "off"
    public static var voices: [DirectCallFace] { faces }
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
    @ObservationIgnored public var onFaceChanged: (@MainActor (String) -> Void)?
    @ObservationIgnored public var onBackgroundChanged: (@MainActor (String, String) -> Void)?
    @ObservationIgnored public var onVoiceChanged: (@MainActor (String) -> Void)?

    public private(set) var conversation = ""
    public private(set) var callId = ""
    public private(set) var partner = ""
    public private(set) var media: DirectCallKind = .audio
    public private(set) var phase: DirectCallPhase?
    public private(set) var invite: DirectCallInvite?
    public private(set) var muted = false
    public private(set) var cameraOff = false
    public private(set) var face = ""
    public private(set) var background = ""
    public private(set) var backgroundImage = ""
    public private(set) var voiceChoice = DirectCallEngine.voiceMatchFace
    public private(set) var voice = ""

    @ObservationIgnored private var caller = false
    @ObservationIgnored private var peerId = ""
    @ObservationIgnored private var peer: DirectCallPeer?
    @ObservationIgnored private var pendingIce: [JSONObject] = []
    @ObservationIgnored private var remoteReady = false
    @ObservationIgnored private var ringTask: Task<Void, Never>?
    @ObservationIgnored private var inviteTask: Task<Void, Never>?
    @ObservationIgnored private var staleTask: Task<Void, Never>?

    public init(
        selfId: @escaping @MainActor () -> String,
        accountName: @escaping @MainActor () -> String,
        partnerFor: @escaping @MainActor (String) -> String,
        sendSignal: @escaping @MainActor (String, String, JSONObject) async -> Bool,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.selfId = selfId
        self.accountName = accountName
        self.partnerFor = partnerFor
        self.sendSignal = sendSignal
        self.sleep = sleep
    }

    public var available: Bool { peerFactory != nil }

    public var active: Bool { phase != nil }

    public var stateLine: String {
        switch phase {
        case .ringing: return "Calling @\(partner)…"
        case .connecting: return "Connecting…"
        case .live: return "Connected · encrypted peer-to-peer"
        case nil: return ""
        }
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
            let missedConversation = self.conversation
            let missedMedia = self.media
            await self.end(notifyPeer: true, reason: "missed")
            self.onMissed?(missedConversation, missedMedia)
        }
        return true
    }

    public func acceptInvite() async {
        guard let pending = invite, !active, available else { return }
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
        _ = await sendSignal(
            pending.conversation, "call-accept", ["call": .string(pending.callId), "to": .string(pending.from)])
    }

    public func declineInvite() async {
        guard let pending = invite else { return }
        clearInvite()
        _ = await sendSignal(
            pending.conversation, "call-decline",
            ["call": .string(pending.callId), "to": .string(pending.from), "reason": "declined"])
    }

    public func hangUp() async {
        await end(notifyPeer: true, reason: "")
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

    public func setFace(_ id: String) {
        let next = DirectCallEngine.faces.contains { $0.id == id } ? id : ""
        guard next == id, next != face else { return }
        face = next
        onFaceChanged?(next)
        if media == .video { peer?.setFace(next) }
        if voiceChoice == DirectCallEngine.voiceMatchFace { pushVoice() }
    }

    public func setBackground(_ id: String, image: String = "") {
        guard DirectCallEngine.backgrounds.contains(where: { $0.id == id }) else { return }
        let custom = id == DirectCallEngine.customBackground
        let picture = custom && !image.isEmpty ? image : backgroundImage
        if custom && picture.isEmpty { return }
        guard id != background || picture != backgroundImage else { return }
        background = id
        backgroundImage = picture
        onBackgroundChanged?(id, picture)
        if media == .video { peer?.setBackground(id, image: custom ? picture : "") }
    }

    public static func backgroundLabel(_ id: String) -> String {
        backgrounds.first { $0.id == id }?.label ?? "No background"
    }

    public var effectiveVoice: String {
        switch voiceChoice {
        case DirectCallEngine.voiceOff: return ""
        case DirectCallEngine.voiceMatchFace: return face
        default: return voiceChoice
        }
    }

    public static func voiceLabel(_ id: String) -> String {
        voices.first { $0.id == id }?.label ?? "Your own"
    }

    public func setVoiceChoice(_ choice: String) {
        let valid = choice == DirectCallEngine.voiceOff || choice == DirectCallEngine.voiceMatchFace
            || DirectCallEngine.voices.contains { $0.id == choice }
        guard valid, choice != voiceChoice else { return }
        voiceChoice = choice
        onVoiceChanged?(choice)
        pushVoice()
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
            guard active, callId == frameCall, caller, peerId.isEmpty else { return }
            await connectAsCaller(to: from)
        case "call-decline":
            guard active, callId == frameCall, caller else { return }
            await end(notifyPeer: false, reason: "")
        case "call-offer":
            guard active, callId == frameCall, !caller, from == peerId,
                let sdp = plain["sdp"]?.objectValue
            else { return }
            await connectAsCallee(offer: sdp)
        case "call-answer":
            guard active, callId == frameCall, caller, from == peerId, let peer,
                let sdp = plain["sdp"]?.objectValue
            else { return }
            do {
                try await peer.acceptAnswer(sdp)
                remoteReady = true
                await flushPendingIce()
            } catch {
                await end(notifyPeer: true, reason: "")
            }
        case "call-ice":
            guard active, callId == frameCall, from == peerId, let candidate = plain["ice"]?.objectValue else { return }
            if remoteReady, let peer {
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
            guard let self, self.invite?.callId == id else { return }
            self.clearInvite()
        }
    }

    private func connectAsCaller(to deviceId: String) async {
        stopRinging()
        peerId = deviceId
        phase = .connecting
        guard let factory = peerFactory else {
            await end(notifyPeer: true, reason: "")
            return
        }
        do {
            let made = try await factory.makePeer(media: media, iceServers: DirectCallEngine.iceServers)
            attach(made)
            let sdp = try await made.offer()
            _ = await sendSignal(
                conversation, "call-offer",
                ["call": .string(callId), "to": .string(deviceId), "sdp": .object(sdp)])
        } catch {
            await end(notifyPeer: true, reason: "")
        }
    }

    private func connectAsCallee(offer: JSONObject) async {
        guard peer == nil, let factory = peerFactory else { return }
        do {
            let made = try await factory.makePeer(media: media, iceServers: DirectCallEngine.iceServers)
            attach(made)
            let answer = try await made.answer(offer: offer)
            remoteReady = true
            await flushPendingIce()
            _ = await sendSignal(
                conversation, "call-answer",
                ["call": .string(callId), "to": .string(peerId), "sdp": .object(answer)])
        } catch {
            await end(notifyPeer: true, reason: "")
        }
    }

    private func attach(_ made: DirectCallPeer) {
        peer = made
        if media == .video && !face.isEmpty { made.setFace(face) }
        if media == .video && !background.isEmpty {
            made.setBackground(background, image: background == DirectCallEngine.customBackground ? backgroundImage : "")
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
        invite = nil
    }

    private func end(notifyPeer: Bool, reason: String) async {
        guard active else { return }
        stopRinging()
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
        closing?.close()
        if notifyPeer {
            var extra: JSONObject = ["call": .string(id)]
            if !reason.isEmpty { extra["reason"] = .string(reason) }
            _ = await sendSignal(scope, "call-bye", extra)
        }
    }
}
