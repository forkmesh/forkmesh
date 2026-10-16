import Foundation
import Observation

public struct FaceChangerOption: Equatable, Sendable, Identifiable {
    public let id: String
    public let label: String

    public init(id: String, label: String) {
        self.id = id
        self.label = label
    }
}

public enum FaceChangerPart: Equatable, Sendable {
    case face
    case background
    case voice
}

@MainActor
@Observable
public final class FaceChangerLook {
    public static let faceKey = "chat.callFace"
    public static let backgroundKey = "chat.callBackground"
    public static let voiceKey = "chat.callVoice"
    public static let faces: [FaceChangerOption] = [
        FaceChangerOption(id: "maya", label: "Maya"),
        FaceChangerOption(id: "theo", label: "Theo"),
        FaceChangerOption(id: "kofi", label: "Kofi"),
        FaceChangerOption(id: "ingrid", label: "Ingrid"),
        FaceChangerOption(id: "ren", label: "Ren"),
    ]
    public static let customFace = "custom"
    public static let faceChoices: [FaceChangerOption] = faces + [FaceChangerOption(id: customFace, label: "Your photo")]
    public static let customBackground = "custom"
    public static let backgrounds: [FaceChangerOption] = [
        FaceChangerOption(id: "", label: "No background"),
        FaceChangerOption(id: "blur", label: "Blur"),
        FaceChangerOption(id: "blur-strong", label: "Strong blur"),
        FaceChangerOption(id: "world", label: "ForkMesh World"),
        FaceChangerOption(id: "office", label: "ForkMesh office"),
        FaceChangerOption(id: "meeting", label: "Meeting room"),
        FaceChangerOption(id: "library", label: "Home library"),
        FaceChangerOption(id: "skyline", label: "City window"),
        FaceChangerOption(id: "custom", label: "Your photo"),
    ]
    public static let voiceMatchFace = "face"
    public static let voiceOff = "off"
    public static let voices: [FaceChangerOption] = [
        FaceChangerOption(id: "maya", label: "Maya"),
        FaceChangerOption(id: "theo", label: "Theo"),
        FaceChangerOption(id: "kofi", label: "Kofi"),
        FaceChangerOption(id: "ingrid", label: "Ingrid"),
        FaceChangerOption(id: "ren", label: "Ren"),
        FaceChangerOption(id: "deep", label: "Deep man"),
        FaceChangerOption(id: "older-man", label: "Older man"),
        FaceChangerOption(id: "young-man", label: "Young man"),
        FaceChangerOption(id: "warm", label: "Warm woman"),
        FaceChangerOption(id: "low", label: "Low woman"),
        FaceChangerOption(id: "older-woman", label: "Older woman"),
        FaceChangerOption(id: "teen", label: "Teen"),
        FaceChangerOption(id: "neutral", label: "Neutral"),
        FaceChangerOption(id: "robot", label: "Robot"),
    ]

    public static func faceLabel(_ id: String) -> String {
        faceChoices.first { $0.id == id }?.label ?? "Your own face"
    }

    public static func backgroundLabel(_ id: String) -> String {
        backgrounds.first { $0.id == id }?.label ?? "No background"
    }

    public static func voiceLabel(_ id: String) -> String {
        voices.first { $0.id == id }?.label ?? "Your own"
    }

    @ObservationIgnored public var onFaceChanged: (@MainActor (String) -> Void)?
    @ObservationIgnored public var onFaceImageChanged: (@MainActor (String) -> Void)?
    @ObservationIgnored public var onBackgroundChanged: (@MainActor (String, String) -> Void)?
    @ObservationIgnored public var onVoiceChanged: (@MainActor (String) -> Void)?
    @ObservationIgnored private var listeners: [UUID: @MainActor (FaceChangerPart) -> Void] = [:]

    public private(set) var face = ""
    public private(set) var faceImage = ""
    public private(set) var background = ""
    public private(set) var backgroundImage = ""
    public private(set) var voiceChoice = FaceChangerLook.voiceMatchFace

    public init() {}

    public var effectiveVoice: String {
        switch voiceChoice {
        case FaceChangerLook.voiceOff: return ""
        case FaceChangerLook.voiceMatchFace: return face == FaceChangerLook.customFace ? "" : face
        default: return voiceChoice
        }
    }

    public var customFaceImage: String { face == FaceChangerLook.customFace ? faceImage : "" }

    public var customBackgroundImage: String { background == FaceChangerLook.customBackground ? backgroundImage : "" }

    @discardableResult
    public func addListener(_ listener: @escaping @MainActor (FaceChangerPart) -> Void) -> UUID {
        let token = UUID()
        listeners[token] = listener
        return token
    }

    public func removeListener(_ token: UUID) {
        listeners[token] = nil
    }

    private func changed(_ part: FaceChangerPart) {
        for listener in listeners.values { listener(part) }
    }

    public func setFace(_ id: String, image: String = "") {
        let custom = id == FaceChangerLook.customFace
        let picture = custom && !image.isEmpty ? image : faceImage
        let known = id.isEmpty || FaceChangerLook.faces.contains { $0.id == id } || (custom && !picture.isEmpty)
        guard known, id != face || picture != faceImage else { return }
        let changedFace = id != face
        face = id
        if picture != faceImage {
            faceImage = picture
            onFaceImageChanged?(picture)
        }
        if changedFace { onFaceChanged?(id) }
        changed(.face)
    }

    public func setBackground(_ id: String, image: String = "") {
        guard FaceChangerLook.backgrounds.contains(where: { $0.id == id }) else { return }
        let custom = id == FaceChangerLook.customBackground
        let picture = custom && !image.isEmpty ? image : backgroundImage
        if custom && picture.isEmpty { return }
        guard id != background || picture != backgroundImage else { return }
        background = id
        backgroundImage = picture
        onBackgroundChanged?(id, picture)
        changed(.background)
    }

    public func setVoiceChoice(_ choice: String) {
        let valid = choice == FaceChangerLook.voiceOff || choice == FaceChangerLook.voiceMatchFace
            || FaceChangerLook.voices.contains { $0.id == choice }
        guard valid, choice != voiceChoice else { return }
        voiceChoice = choice
        onVoiceChanged?(choice)
        changed(.voice)
    }
}
