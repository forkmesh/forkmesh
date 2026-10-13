import FMApi
import FMBase
import Foundation

public enum ZipCRC32 {
    static let table: [UInt32] = (0..<256).map { n -> UInt32 in
        var c = UInt32(n)
        for _ in 0..<8 { c = (c & 1) != 0 ? 0xEDB8_8320 ^ (c >> 1) : c >> 1 }
        return c
    }

    public static func checksum<D: Sequence>(_ bytes: D) -> UInt32 where D.Element == UInt8 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in bytes { crc = table[Int((crc ^ UInt32(byte)) & 0xFF)] ^ (crc >> 8) }
        return crc ^ 0xFFFF_FFFF
    }
}

public struct ZipStoreWriter {
    public static let maxBytes = 64 * 1024 * 1024
    public static let maxEntries = 5000

    struct Central {
        let name: [UInt8]
        let crc: UInt32
        let size: UInt32
        let offset: UInt32
    }

    public private(set) var data = Data()
    var central: [Central] = []
    let dosTime: UInt16
    let dosDate: UInt16

    public init(date: Date = Date()) {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second], from: date)
        let year: Int = max(1980, c.year ?? 1980)
        let hour: Int = c.hour ?? 0
        let minute: Int = c.minute ?? 0
        let second: Int = c.second ?? 0
        let month: Int = c.month ?? 1
        let day: Int = c.day ?? 1
        let time: Int = (hour << 11) | (minute << 5) | (second / 2)
        let date: Int = ((year - 1980) << 9) | (month << 5) | day
        dosTime = UInt16(truncatingIfNeeded: time)
        dosDate = UInt16(truncatingIfNeeded: date)
    }

    public var count: Int { central.count }

    func u16(_ value: UInt16, into out: inout Data) {
        out.append(UInt8(value & 0xFF))
        out.append(UInt8(value >> 8))
    }

    func u32(_ value: UInt32, into out: inout Data) {
        for shift in stride(from: 0, to: 32, by: 8) { out.append(UInt8((value >> UInt32(shift)) & 0xFF)) }
    }

    public static func safeName(_ path: String) -> String? {
        let parts = path.split(separator: "/").map(String.init)
        guard !parts.isEmpty, !parts.contains(where: { $0 == ".." || $0 == "." || $0.isEmpty }) else { return nil }
        return parts.joined(separator: "/")
    }

    @discardableResult
    public mutating func add(path: String, bytes: Data) -> Bool {
        guard let clean = ZipStoreWriter.safeName(path), central.count < ZipStoreWriter.maxEntries,
              data.count + bytes.count + 256 < ZipStoreWriter.maxBytes else { return false }
        let name = Array(clean.utf8)
        let crc = ZipCRC32.checksum(bytes)
        let offset = UInt32(data.count)
        var header = Data()
        u32(0x0403_4B50, into: &header)
        u16(20, into: &header)
        u16(0x0800, into: &header)
        u16(0, into: &header)
        u16(dosTime, into: &header)
        u16(dosDate, into: &header)
        u32(crc, into: &header)
        u32(UInt32(bytes.count), into: &header)
        u32(UInt32(bytes.count), into: &header)
        u16(UInt16(name.count), into: &header)
        u16(0, into: &header)
        header.append(contentsOf: name)
        data.append(header)
        data.append(bytes)
        central.append(Central(name: name, crc: crc, size: UInt32(bytes.count), offset: offset))
        return true
    }

    public mutating func finish() -> Data {
        let start = UInt32(data.count)
        var directory = Data()
        for entry in central {
            u32(0x0201_4B50, into: &directory)
            u16(20, into: &directory)
            u16(20, into: &directory)
            u16(0x0800, into: &directory)
            u16(0, into: &directory)
            u16(dosTime, into: &directory)
            u16(dosDate, into: &directory)
            u32(entry.crc, into: &directory)
            u32(entry.size, into: &directory)
            u32(entry.size, into: &directory)
            u16(UInt16(entry.name.count), into: &directory)
            u16(0, into: &directory)
            u16(0, into: &directory)
            u16(0, into: &directory)
            u16(0, into: &directory)
            u32(0, into: &directory)
            u32(entry.offset, into: &directory)
            directory.append(contentsOf: entry.name)
        }
        var end = Data()
        u32(0x0605_4B50, into: &end)
        u16(0, into: &end)
        u16(0, into: &end)
        u16(UInt16(central.count), into: &end)
        u16(UInt16(central.count), into: &end)
        u32(UInt32(directory.count), into: &end)
        u32(start, into: &end)
        u16(0, into: &end)
        var out = data
        out.append(directory)
        out.append(end)
        return out
    }
}

@MainActor
public protocol RepoArchiveReading: AnyObject {
    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree
    func rawBytes(_ owner: String, _ name: String, _ path: String, ref: String, maxBytes: Int) async throws -> Data
}

extension ApiService: RepoArchiveReading {}

public struct RepoArchiveResult: Equatable, Sendable {
    public let data: Data
    public let files: Int
    public let skipped: [String]
    public let fileName: String
}

@MainActor
public final class RepoArchiveBuilder {
    public nonisolated static let maxFileBytes = 8 * 1024 * 1024
    public nonisolated static let maxDirectories = 2000

    let reader: RepoArchiveReading
    public private(set) var progress: (files: Int, bytes: Int) = (0, 0)

    public init(reader: RepoArchiveReading) { self.reader = reader }

    public nonisolated static func fileName(repo: String, ref: String) -> String {
        let clean = ref.isEmpty ? "" : "-" + ref.replacingOccurrences(of: "/", with: "-")
        return "\(repo)\(clean).zip"
    }

    public func build(owner: String, name: String, ref: String, onProgress: ((Int, Int) -> Void)? = nil) async throws -> RepoArchiveResult {
        var writer = ZipStoreWriter()
        var skipped: [String] = []
        var queue = [""]
        var visited = 0
        let root = ref.isEmpty ? name : "\(name)-\(ref.replacingOccurrences(of: "/", with: "-"))"
        while let dir = queue.first {
            queue.removeFirst()
            visited += 1
            if visited > RepoArchiveBuilder.maxDirectories {
                skipped.append(dir)
                continue
            }
            try Task.checkCancellation()
            let listing = try await reader.tree(owner, name, path: dir, ref: ref)
            for entry in listing.entries.sorted(by: { $0.path < $1.path }) {
                if entry.isDirectory {
                    queue.append(entry.path)
                    continue
                }
                if entry.size > RepoArchiveBuilder.maxFileBytes {
                    skipped.append(entry.path)
                    continue
                }
                guard let bytes = try? await reader.rawBytes(owner, name, entry.path, ref: ref, maxBytes: RepoArchiveBuilder.maxFileBytes),
                      writer.add(path: "\(root)/\(entry.path)", bytes: bytes) else {
                    skipped.append(entry.path)
                    continue
                }
                progress = (writer.count, writer.data.count)
                onProgress?(writer.count, writer.data.count)
            }
        }
        let files = writer.count
        return RepoArchiveResult(
            data: writer.finish(), files: files, skipped: skipped, fileName: RepoArchiveBuilder.fileName(repo: name, ref: ref))
    }
}
