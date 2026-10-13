import FMApi
import FMBase
import Foundation

public struct LocalAgentTranscript: Equatable, Sendable {
    public let text: String
    public let attachments: [AgentAttachmentRef]

    public init(text: String, attachments: [AgentAttachmentRef]) {
        self.text = text
        self.attachments = attachments
    }

    public static let idPattern = "^[0-9a-f]{32}$"

    public static let maxAttachmentBytes = 3 * 1024 * 1024

    public static func fromJSON(_ json: JSONObject, sessionId: Int) -> LocalAgentTranscript {
        let text = json.str("transcript")
        var refs = AgentAttachmentMeta.list(json["attachments"], idPattern: idPattern).map {
            AgentAttachmentRef(meta: $0, origin: .desktop(sessionId: sessionId))
        }
        if json["attachments"] == nil {
            refs = AgentTranscriptAttachments.references(in: text)
        }
        return LocalAgentTranscript(text: text, attachments: refs)
    }

    public static func attachmentBytes(_ json: JSONObject) throws -> Data {
        let encoded = json.str("data")
        if encoded.isEmpty || encoded.count > (maxAttachmentBytes + 2) / 3 * 4 + 4 {
            throw AgentAttachmentError("The desktop sent no usable picture.")
        }
        guard let data = Data(base64Encoded: encoded) else {
            throw AgentAttachmentError("The desktop sent a damaged picture.")
        }
        _ = try AgentAttachmentMedia.checkImage(data, maxBytes: maxAttachmentBytes)
        return data
    }

    public static let maxPreviewBytes = 700 * 1024

    public static func attachmentParams(sessionId: Int, attachmentId: String, preview: Bool) -> JSONObject {
        var params: JSONObject = ["id": .int(Int64(sessionId)), "attachment": .string(attachmentId)]
        if preview { params["preview"] = .bool(true) }
        return params
    }

    public static func attachmentPayload(_ json: JSONObject) throws -> AgentAttachmentPayload {
        let data = try attachmentBytes(json)
        guard json["preview"] == .bool(true) else { return .full(data) }
        if data.count > maxPreviewBytes { throw AgentAttachmentError("The desktop sent a preview that is too large.") }
        let original = ModelJSON.int(json["originalSize"], fallback: -1)
        let width = ModelJSON.int(json["width"], fallback: 0)
        let height = ModelJSON.int(json["height"], fallback: 0)
        return AgentAttachmentPayload(
            data: data, preview: true, originalSize: max(-1, original), width: max(0, width), height: max(0, height))
    }

    public static func attachmentErrorText(_ code: String) -> String {
        switch code {
        case "not_connected", "closed": return "Connect to the desktop on this network to open this attachment."
        case "unknown_attachment", "unknown_session": return "The desktop no longer has this attachment."
        case "not_an_image": return "The desktop can only show pictures here."
        case "too_large": return "This file is too large to show on the phone."
        case "too_large_over_relay": return "This file is too large to open through the relay. Open it on the desktop's network."
        case "unsupported": return "Update ForkMesh on the desktop to see attachments here."
        case "timeout": return "The desktop did not answer in time."
        default: return "The desktop could not send this attachment (\(code))."
        }
    }
}
