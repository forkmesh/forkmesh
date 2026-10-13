import Foundation

public struct OnDeviceChatAccess: Sendable, Equatable {
    public enum Action: Sendable, Equatable {
        case turnOn
        case download(bytes: Int64)
        case resumeDownload(doneBytes: Int64, totalBytes: Int64)
        case cancelDownload

        public var label: String {
            switch self {
            case .turnOn: return "Turn on"
            case .download(let bytes): return "Download · \(LocalAIFormat.bytes(bytes))"
            case .resumeDownload: return "Resume download"
            case .cancelDownload: return "Cancel"
            }
        }
    }

    public let title: String
    public let isEnabled: Bool
    public let reason: String?
    public let action: Action?
    public let progress: Double?

    public init(title: String, isEnabled: Bool, reason: String?, action: Action?, progress: Double? = nil) {
        self.title = title
        self.isEnabled = isEnabled
        self.reason = reason
        self.action = action
        self.progress = progress
    }

    public static func resolve(
        model: LocalAIModel,
        switchOn: Bool,
        engine: LocalEngineAvailability,
        fit: LocalModelFit,
        state: LocalModelState,
        isDownloaded: Bool,
        resumableBytes: Int64,
        totalBytes: Int64? = nil
    ) -> OnDeviceChatAccess {
        let title = "Chat with \(model.displayName)"
        let name = model.displayName
        let total = max(totalBytes ?? model.approxDownloadBytes, 0)
        func blocked(_ reason: String, _ action: Action?, progress: Double? = nil) -> OnDeviceChatAccess {
            OnDeviceChatAccess(title: title, isEnabled: false, reason: reason, action: action, progress: progress)
        }
        if let reason = engine.reason {
            return blocked("Not supported on this device. \(reason)", nil)
        }
        switch fit {
        case .tooLarge(let reason), .unsupported(let reason):
            return blocked(reason, nil)
        case .recommended, .tight:
            break
        }
        if !switchOn {
            if isDownloaded {
                return blocked("On-device AI is off.", .turnOn)
            }
            return blocked("On-device AI is off, and \(name) is not downloaded yet.", .turnOn)
        }
        if !isDownloaded {
            let resume: Action = resumableBytes > 0
                ? .resumeDownload(doneBytes: resumableBytes, totalBytes: total)
                : .download(bytes: total)
            switch state {
            case .downloading(let progress, let bytes):
                let clamped = min(max(progress.isFinite ? progress : 0, 0), 1)
                return blocked(
                    "Downloading \(name): \(LocalAIFormat.percent(clamped)) · \(LocalAIFormat.bytes(bytes)) of \(LocalAIFormat.bytes(total)).",
                    .cancelDownload,
                    progress: clamped
                )
            case .failed(let message):
                return blocked(message, resume)
            default:
                if resumableBytes > 0 {
                    return blocked("\(name) is partly downloaded: \(LocalAIFormat.bytes(resumableBytes)) of \(LocalAIFormat.bytes(total)).", resume)
                }
                return blocked("\(name) is not downloaded yet (\(LocalAIFormat.bytes(total))).", resume)
            }
        }
        if case .failed(let message) = state {
            return OnDeviceChatAccess(title: title, isEnabled: true, reason: message, action: nil)
        }
        return OnDeviceChatAccess(title: title, isEnabled: true, reason: nil, action: nil)
    }
}

extension LocalModelManager {
    public var chatAccess: OnDeviceChatAccess {
        let model = selectedModel
        return OnDeviceChatAccess.resolve(
            model: model,
            switchOn: isEnabled,
            engine: engineAvailability,
            fit: fit(for: model),
            state: state(for: model),
            isDownloaded: isDownloaded(model),
            resumableBytes: resumableBytes[model.id] ?? 0,
            totalBytes: downloadProgress[model.id]?.totalBytes
        )
    }

    public func performChatAccess(_ action: OnDeviceChatAccess.Action) {
        let model = selectedModel
        switch action {
        case .turnOn:
            setEnabled(true)
        case .download, .resumeDownload:
            download(model)
        case .cancelDownload:
            cancelDownload(model)
        }
    }
}
