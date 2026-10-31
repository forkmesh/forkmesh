import FMBase
import Foundation

public struct GroupCallFmmAccess: Equatable, Sendable {
    public static let alpn = "fmm/1"

    public let json: String
    public let addr: String
    public let hasStandby: Bool

    public init?(json value: JSONValue?) {
        guard let object = value?.objectValue, Self.valid(object) else { return nil }
        if let engine = object["engine"]?.text, engine != "fmm" { return nil }
        var block = object
        var standby = false
        if let inner = object["standby"]?.objectValue {
            if Self.valid(inner) {
                standby = true
            } else {
                block.removeValue(forKey: "standby")
            }
        }
        self.json = JSON.encode(block)
        self.addr = object["addr"]?.text ?? ""
        self.hasStandby = standby
    }

    static func valid(_ object: JSONObject) -> Bool {
        let addr = object["addr"]?.text ?? ""
        let ticket = object["ticket"]?.text ?? ""
        let alpn = object["alpn"]?.text ?? alpn
        let hashes = object["hashes"]?.arrayValue?.compactMap { $0.text }.filter { !$0.isEmpty } ?? []
        return !addr.isEmpty && addr.contains(":") && !ticket.isEmpty && alpn == Self.alpn && !hashes.isEmpty
    }
}

public struct GroupCallIdentity: Sendable {
    public let call: String
    public let acct: String
    public let kid: String
    public let seed: [UInt8]

    public init(call: String, acct: String, kid: String, seed: [UInt8]) {
        self.call = call
        self.acct = acct
        self.kid = kid
        self.seed = seed
    }
}

public enum GroupCallEngine: String, Sendable {
    case fmm
    case webrtc

    public static let request: JSONObject = ["engines": .array([.string("fmm"), .string("webrtc")])]
    public static let refusedByRoom = 409
}

public enum GroupCallE2eeKind: String, Sendable {
    case presence
    case key
    case verify
}

public struct GroupCallE2eeMessage: Equatable, Sendable {
    public let kind: GroupCallE2eeKind
    public let peer: String
    public let data: Data
    public let senderKid: String
    public let senderAcct: String
}

public enum GroupCallE2ee {
    public static let field = "e2ee"
    public static let sealKid = "sealKid"
    public static let sealAcct = "sealAcct"

    public static func fields(_ kind: GroupCallE2eeKind, peer: String, message: Data, state: String = "in", media: String = "") -> JSONObject? {
        guard let parsed = try? JSON.parse(message), parsed.objectValue != nil else { return nil }
        var out: JSONObject
        switch kind {
        case .presence:
            out = GroupCallFrames.presence(peer: peer, state: state, media: media)
        case .key, .verify:
            out = ["peer": .string(peer)]
        }
        out[field] = parsed
        return out
    }

    public static func type(_ kind: GroupCallE2eeKind) -> String {
        switch kind {
        case .presence: return GroupCallFrames.presenceType
        case .key: return GroupCallFrames.keyType
        case .verify: return GroupCallFrames.verifyType
        }
    }

    public static func message(_ plain: JSONObject) -> GroupCallE2eeMessage? {
        guard let type = plain["type"]?.text, let payload = plain[field], payload.objectValue != nil else { return nil }
        let kind: GroupCallE2eeKind
        switch type {
        case GroupCallFrames.presenceType: kind = .presence
        case GroupCallFrames.keyType: kind = .key
        case GroupCallFrames.verifyType: kind = .verify
        default: return nil
        }
        let kid = plain[sealKid]?.text ?? ""
        guard !kid.isEmpty else { return nil }
        return GroupCallE2eeMessage(
            kind: kind, peer: plain["peer"]?.text ?? "", data: Data(JSON.encode(payload).utf8), senderKid: kid,
            senderAcct: plain[sealAcct]?.text ?? "")
    }

    public static func annotate(_ plain: JSONObject, kid: String?, acct: String?) -> JSONObject {
        var out = plain
        out.removeValue(forKey: sealKid)
        out.removeValue(forKey: sealAcct)
        guard let type = out["type"]?.text, GroupCallFrames.types.contains(type) else { return out }
        if let kid, !kid.isEmpty { out[sealKid] = .string(kid) }
        if let acct, !acct.isEmpty { out[sealAcct] = .string(acct) }
        return out
    }
}

public enum GroupCallAuthState: Int, Sendable {
    case pending = 0
    case verified = 1
    case missing = 2
    case forged = 3
}

public enum GroupCallVerifyText {
    public static let title = "Compare these with everyone in the call"
    public static let match = "They match"
    public static let noMatch = "They don't match"
    public static let scan = "Scan a code instead"
    public static let mismatch = "Someone in this call may not be who they seem. Leave the call or ask everyone to check their devices."
    public static let interference = "Verification keeps restarting. Someone may be interfering with this call. Scan each other's codes if you can."
    public static let verified = "Everyone in this call is verified."
    public static let preparing = "The verification code appears once everyone's devices have exchanged keys."
    public static let maxUnfinished = 3
    public static let maxRounds = 32

    public static func numbers(_ digits: String) -> String { "Or compare numbers: \(digits)" }

    public static func waiting(_ names: [String]) -> String { "Waiting for \(list(names)) to confirm…" }

    public static func changed(_ name: String) -> String { "The verification code changed because \(name) joined" }

    public static func newcomer(_ name: String) -> String { "\(name) is not verified. Compare the new code to verify everyone." }

    public static func missing(_ name: String) -> String { "Can't confirm this is \(name) right now" }

    public static func forged(_ name: String) -> String { "\(name)'s video failed a check. Someone in this call may be faking it." }

    public static func interfered(unfinished: Int, rounds: Int) -> Bool { unfinished >= maxUnfinished || rounds > maxRounds }

    public static func list(_ names: [String]) -> String {
        switch names.count {
        case 0: return ""
        case 1: return names[0]
        default: return names.dropLast().joined(separator: ", ") + " and " + (names.last ?? "")
        }
    }

    public static func tileNote(_ state: GroupCallAuthState, name: String) -> String? {
        switch state {
        case .missing: return missing(name)
        case .forged: return forged(name)
        case .pending, .verified: return nil
        }
    }
}

public struct GroupCallAudioPlan: Equatable, Sendable {
    public let bitrate: Int
    public let fec: Bool
    public let lossPercent: Int
    public let dredFrames: Int
    public let depthMs: Int

    public init(bitrate: Int, fec: Bool, lossPercent: Int, dredFrames: Int, depthMs: Int) {
        self.bitrate = bitrate
        self.fec = fec
        self.lossPercent = lossPercent
        self.dredFrames = dredFrames
        self.depthMs = depthMs
    }

    public static let frameSamples = 960
    public static let sampleRate = 48000
    public static let lossyBitrate = 48000
    public static let lossyFloor = 0.01
    public static let lossyTarget = 96000

    public static func make(engineBitrate: Int, targetBitrate: Int, redundancyMs: Int, loss: Double) -> GroupCallAudioPlan {
        let lossy = loss >= lossyFloor || redundancyMs > 200
        var bitrate = max(engineBitrate, 12000)
        if lossy && targetBitrate >= lossyTarget { bitrate = max(bitrate, lossyBitrate) }
        let percent = lossy ? min(max(Int((loss * 100).rounded(.up)), 5), 50) : 0
        let dred = lossy ? min(max(redundancyMs, 0), 1000) / 10 : 0
        let depth = min(dred * 10, carried(bitrate))
        return GroupCallAudioPlan(bitrate: bitrate, fec: lossy, lossPercent: percent, dredFrames: dred, depthMs: depth)
    }

    public static func carried(_ bitrate: Int) -> Int {
        switch bitrate {
        case ..<30000: return 0
        case ..<44000: return 80
        default: return 400
        }
    }
}

public enum GroupCallAudioStep: Equatable, Sendable {
    case silence
    case play
    case dred(samples: Int)
    case fec
    case conceal
}

public enum GroupCallAudioDecode {
    public static let waiting = 0
    public static let play = 1
    public static let conceal = 2
    public static let insert = 3
    public static let dred = 4

    public static func step(status: Int, offset: Int, hasData: Bool, hasLBRR: Bool, dredSamples: Int) -> GroupCallAudioStep {
        switch status {
        case play:
            return hasData ? .play : .conceal
        case dred:
            guard hasData, offset > 0 else { return .conceal }
            if offset == 1 && hasLBRR { return .fec }
            let samples = offset * GroupCallAudioPlan.frameSamples
            return dredSamples >= samples ? .dred(samples: samples) : (offset == 1 && hasLBRR ? .fec : .conceal)
        case conceal:
            return hasData && hasLBRR ? .fec : .conceal
        case insert:
            return .conceal
        default:
            return .silence
        }
    }
}

public struct GroupCallVideoLayer: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let fps: Int
    public let share: Double

    public static let simulcast: [GroupCallVideoLayer] = [
        GroupCallVideoLayer(width: 320, height: 180, fps: 15, share: 0.25),
        GroupCallVideoLayer(width: 640, height: 360, fps: 24, share: 0.75),
    ]
    public static let temporalLayers = 2
    public static let keyframeEverySeconds = 10
}

public struct GroupCallLongTermRefs: Sendable {
    public static let keep = 32

    private var tokens: [(frame: Int64, token: Int64)] = []

    public init() {}

    public mutating func note(frame: Int64, token: Int64) {
        tokens.append((frame, token))
        if tokens.count > Self.keep { tokens.removeFirst(tokens.count - Self.keep) }
    }

    public mutating func reset() { tokens.removeAll() }

    public func token(for frame: Int64) -> Int64? {
        tokens.last { $0.frame == frame }?.token
    }

    public func acknowledged(upTo frame: Int64) -> [Int64] {
        guard frame >= 0 else { return [] }
        return tokens.filter { $0.frame <= frame }.suffix(4).map(\.token)
    }

    public var isEmpty: Bool { tokens.isEmpty }
}

public enum H264Bitstream {
    public static let startCode: [UInt8] = [0, 0, 0, 1]

    public static func annexB(avcc: [UInt8], lengthSize: Int = 4, parameterSets: [[UInt8]] = []) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(avcc.count + parameterSets.reduce(0) { $0 + $1.count + 4 })
        for set in parameterSets {
            out += startCode
            out += set
        }
        var index = 0
        while index + lengthSize <= avcc.count {
            var length = 0
            for byte in avcc[index..<(index + lengthSize)] { length = length << 8 | Int(byte) }
            index += lengthSize
            guard length > 0, index + length <= avcc.count else { break }
            out += startCode
            out += avcc[index..<(index + length)]
            index += length
        }
        return out
    }

    public static func units(annexB data: [UInt8]) -> [ArraySlice<UInt8>] {
        var starts: [(at: Int, code: Int)] = []
        var index = 0
        while index + 2 < data.count {
            if data[index] == 0 && data[index + 1] == 0 {
                if data[index + 2] == 1 {
                    starts.append((index, 3))
                    index += 3
                    continue
                }
                if index + 3 < data.count && data[index + 2] == 0 && data[index + 3] == 1 {
                    starts.append((index, 4))
                    index += 4
                    continue
                }
            }
            index += 1
        }
        var out: [ArraySlice<UInt8>] = []
        for (n, start) in starts.enumerated() {
            let begin = start.at + start.code
            var end = n + 1 < starts.count ? starts[n + 1].at : data.count
            while end > begin && data[end - 1] == 0 && n + 1 < starts.count { end -= 1 }
            if end > begin { out.append(data[begin..<end]) }
        }
        return out
    }

    public static func type(_ unit: ArraySlice<UInt8>) -> Int { Int((unit.first ?? 0) & 0x1F) }

    public static func avcc(_ units: [ArraySlice<UInt8>]) -> [UInt8] {
        var out: [UInt8] = []
        for unit in units {
            let length = UInt32(unit.count)
            out += [UInt8(length >> 24), UInt8(length >> 16 & 0xFF), UInt8(length >> 8 & 0xFF), UInt8(length & 0xFF)]
            out += unit
        }
        return out
    }

    public static let sps = 7
    public static let pps = 8
    public static let idr = 5
}

public struct GroupCallVerifyState: Equatable, Sendable {
    public let round: String
    public let hasCode: Bool
    public let emoji: [String]
    public let names: [String]
    public let digits: String
    public let waiting: [String]
    public let late: Bool
    public let verified: Bool
    public let newcomers: [String]
    public let interfered: Bool

    public init(
        round: String, hasCode: Bool, emoji: String, names: String, digits: String, waiting: String, late: Bool,
        verified: Bool, newcomers: String, unfinished: Int, rounds: Int
    ) {
        self.round = round
        self.hasCode = hasCode
        self.emoji = emoji.split(separator: " ").map(String.init)
        self.names = names.components(separatedBy: ", ").filter { !$0.isEmpty }
        self.digits = digits
        self.waiting = waiting.split(separator: "\n").map(String.init)
        self.late = late
        self.verified = verified
        self.newcomers = newcomers.split(separator: "\n").map(String.init)
        self.interfered = GroupCallVerifyText.interfered(unfinished: unfinished, rounds: rounds)
    }

    public func summary(name: (String) -> String) -> String {
        if interfered { return GroupCallVerifyText.interference }
        if verified { return GroupCallVerifyText.verified }
        if let first = newcomers.first, hasCode { return GroupCallVerifyText.newcomer(name(first)) }
        if !waiting.isEmpty && late { return GroupCallVerifyText.waiting(waiting.map(name)) }
        return hasCode ? GroupCallVerifyText.title : GroupCallVerifyText.preparing
    }
}
