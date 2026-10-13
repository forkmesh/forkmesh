import FMApi
import FMBase
import FMChat
import FMCrypto
import Foundation

public enum DesktopPromptImages {
    public static let chunkBytes = 192 * 1024
    public static let relayCharsPerSecond = 100_000

    public typealias Pause = @Sendable (Double) async -> Void

    public static let sleep: Pause = { seconds in
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
    }

    @MainActor
    public static func upload(
        _ images: [PromptAttachment], over link: DesktopControlLink, pause: Pause = sleep
    ) async throws -> [JSONValue] {
        var ids: [String] = []
        do {
            for image in images {
                let bytes = [UInt8](image.bytes)
                let begin = try await link.request(
                    "agents.image.begin",
                    [
                        "mediaType": .string(image.mediaType), "size": .int(Int64(bytes.count)),
                        "sha256": .string(Sha256.hex(bytes)),
                    ],
                    timeout: 20)
                guard case .string(let id)? = begin["image"], !id.isEmpty else {
                    throw LocalControlException("bad_image")
                }
                ids.append(id)
                var offset = 0
                while offset < bytes.count {
                    let end = min(bytes.count, offset + chunkBytes)
                    let encoded = Data(bytes[offset..<end]).base64EncodedString()
                    _ = try await link.request(
                        "agents.image.chunk", ["image": .string(id), "data": .string(encoded)], timeout: 60)
                    offset = end
                    if link.transport == .relay {
                        await pause(Double(encoded.utf8.count) / Double(relayCharsPerSecond))
                    }
                }
            }
        } catch {
            for id in ids {
                _ = try? await link.request("agents.image.cancel", ["image": .string(id)], timeout: 10)
            }
            if (error as? LocalControlException)?.code == "unsupported" {
                throw LocalControlException("images_unsupported")
            }
            throw error
        }
        return ids.map { .string($0) }
    }

    @MainActor
    public static func params(
        _ base: JSONObject, images: [PromptAttachment], over link: DesktopControlLink, pause: Pause = sleep
    ) async throws -> JSONObject {
        if images.isEmpty { return base }
        var params = base
        params["images"] = .array(try await upload(images, over: link, pause: pause))
        return params
    }
}
