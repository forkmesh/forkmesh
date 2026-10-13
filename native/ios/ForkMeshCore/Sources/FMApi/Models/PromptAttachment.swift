// The wire half of prompt attachments: a photo
// attached to a prompt, already prepared (downscaled, re-encoded as JPEG,
// stripped of metadata), and the limits the relay enforces on queued jobs.
// It lives in FMApi because ApiService sends it; the preparation rules that
// make one (metadata strip, prompt block, working-tree path) are in FMAgents'
// PromptAttachments.swift, and the ImageIO downscale is in the app target
// (Platform/PromptPhotoPicker.swift).

import FMBase
import Foundation

/// A photo attached to a prompt, already prepared for sending.
///
/// Only prepared bytes ever reach this type, so nothing downstream (the
/// on-device agent's request, the relay queue) can leak a photo's location or
/// its full-resolution original by accident.
public struct PromptAttachment: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let bytes: Data
    public let mediaType: String
    public let width: Int
    public let height: Int

    public init(id: String, name: String, bytes: Data, mediaType: String = "image/jpeg", width: Int = 0, height: Int = 0) {
        self.id = id
        self.name = name
        self.bytes = bytes
        self.mediaType = mediaType
        self.width = width
        self.height = height
    }

    public var size: Int { bytes.count }

    public var base64Data: String { bytes.base64EncodedString() }

    /// The relay's wire shape for a queued agent job
    /// (`attachments: [{name, mediaType, data}]`, org_agent_attachments).
    public var wire: JSONValue {
        .object([
            "name": .string(name),
            "mediaType": .string(mediaType),
            "data": .string(base64Data),
        ])
    }
}

/// Why a picked file could not become an attachment, in words for a person.
public struct PromptAttachmentError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

/// Limits shared by every surface that attaches photos. The relay enforces
/// the same numbers for queued jobs (ORG_AGENT_ATTACHMENT_* in
/// app/src/route_handlers.py); a phone meeting them first means a send is
/// never refused after the upload.
public enum PromptAttachmentLimits {
    public static let maxCount = 5
    public static let maxBytes = 5 * 1024 * 1024
    /// The relay caps one request's photos together, too.
    public static let maxTotalBytes = 10 * 1024 * 1024
    public static let maxDimension = 2048
    public static let jpegQuality = 80
}

/// The prompt to send: what was typed, or, when only photos were attached, a
/// line that points the agent at them, since the relay requires a prompt.
public func promptOrPhotoNote(_ prompt: String, _ attachments: [PromptAttachment]) -> String {
    let text = prompt.dartTrimmed
    if !text.isEmpty || attachments.isEmpty { return text }
    return "See the attached photos."
}

/// The relay's limits for photos on one request, checked before upload. The
/// sentence for a person, or nil when the set may be sent.
public func promptAttachmentsProblem(_ attachments: [PromptAttachment]) -> String? {
    if attachments.count > PromptAttachmentLimits.maxCount {
        return "A prompt can carry \(PromptAttachmentLimits.maxCount) photos at most."
    }
    for item in attachments where item.size > PromptAttachmentLimits.maxBytes {
        return "\(item.name) is larger than 5 MB."
    }
    let total = attachments.reduce(0) { $0 + $1.size }
    if total > PromptAttachmentLimits.maxTotalBytes {
        return "Those photos add up to more than 10 MB. Send fewer at once."
    }
    return nil
}
