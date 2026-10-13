import FMDiff
import Foundation

public enum DiffImageKind: Equatable, Sendable {
    case raster
    case svg

    static let rasterExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "webp", "bmp", "ico", "heic", "tif", "tiff"]

    public static func of(_ path: String) -> DiffImageKind? {
        let name = path.split(separator: "/").last.map(String.init) ?? path
        guard let dot = name.lastIndex(of: "."), dot != name.startIndex else { return nil }
        let ext = name[name.index(after: dot)...].lowercased()
        if ext == "svg" { return .svg }
        return rasterExtensions.contains(ext) ? .raster : nil
    }
}

public enum DiffImageSide: String, Sendable {
    case old
    case new

    public var label: String { self == .old ? "Before" : "After" }
}

public struct DiffImageRequest: Hashable, Sendable {
    public let file: Int
    public let group: String
    public let path: String
    public let side: DiffImageSide

    public init(file: Int, group: String = "", path: String, side: DiffImageSide) {
        self.file = file
        self.group = group
        self.path = path
        self.side = side
    }
}

public enum DiffImageResult: Equatable, Sendable {
    case loading
    case loaded(Data)
    case failed(String)
}

public struct DiffImageError: Error, Equatable, Sendable {
    public let message: String

    public init(_ message: String) { self.message = message }
}

public typealias DiffImageLoader = @MainActor (_ request: DiffImageRequest) async throws -> Data

public enum DiffImages {
    public static let maxBytes = 2 * 1024 * 1024
    public static let unreadable = "This image could not be shown."

    public static func previewable(_ file: DiffFile) -> Bool {
        DiffImageKind.of(file.path) != nil
    }

    public static func requests(_ entry: DiffReviewFile, group: String = "") -> [DiffImageRequest] {
        let file = entry.file
        var sides: [DiffImageRequest] = []
        if file.status != .added {
            sides.append(DiffImageRequest(file: entry.index, group: group, path: file.oldPath, side: .old))
        }
        if file.status != .deleted {
            sides.append(DiffImageRequest(file: entry.index, group: group, path: file.newPath, side: .new))
        }
        return sides.filter { !$0.path.isEmpty }
    }

    public static func describe(_ error: Error) -> String {
        (error as? DiffImageError)?.message ?? unreadable
    }
}
