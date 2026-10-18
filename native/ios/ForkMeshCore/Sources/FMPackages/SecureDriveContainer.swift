import FMCrypto
import Foundation

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

public enum SecureDriveLayout {
    public static let pageBytes = 16384
    public static let unitOverhead = 28
    public static let chunkBytes = pageBytes - unitOverhead
    public static let leasePage: Int64 = 0
    public static let rootPages: Int64 = 4
    public static let rootAPage: Int64 = 1
    public static let rootBPage: Int64 = 5
    public static let mailboxPage: Int64 = 9
    public static let mailboxSlots = 64
    public static let slotBytes = 8192
    public static let dataPage: Int64 = 41
    public static let initialDataPages: Int64 = 32

    static func mailboxOffset(_ mailbox: Int) -> Int64 {
        mailboxPage * Int64(pageBytes) + Int64(mailbox) * Int64(slotBytes)
    }
}

public protocol SecureDriveFile: AnyObject {
    var size: Int64 { get }
    func read(at offset: Int64, count: Int) -> [UInt8]?
    func write(at offset: Int64, _ bytes: [UInt8]) -> Bool
    func sync() -> Bool
}

extension SecureDriveFile {
    var pages: Int64 { size / Int64(SecureDriveLayout.pageBytes) }

    func appendRandom(pages count: Int64) -> Bool {
        var offset = size
        var left = count * Int64(SecureDriveLayout.pageBytes)
        while left > 0 {
            let block = CryptoBytes.random(Int(min(left, 1 << 20)))
            if !write(at: offset, block) { return false }
            offset += Int64(block.count)
            left -= Int64(block.count)
        }
        return sync()
    }
}

public protocol SecureDriveStorage: AnyObject {
    func withFile<T>(_ container: String, writable: Bool, _ body: (SecureDriveFile) throws -> T) throws -> T
    func createEmpty(_ container: String) throws
    func exists(_ container: String) -> Bool
    func removeCreated(_ container: String)
    func canonical(_ container: String) -> String
    func displayName(_ container: String) -> String
}

public final class SecureDriveHandleFile: SecureDriveFile {
    let handle: FileHandle

    public init(handle: FileHandle) {
        self.handle = handle
    }

    public var size: Int64 {
        (try? handle.seekToEnd()).map { Int64($0) } ?? 0
    }

    public func read(at offset: Int64, count: Int) -> [UInt8]? {
        guard offset >= 0, count >= 0 else { return nil }
        do {
            try handle.seek(toOffset: UInt64(offset))
            var out = [UInt8]()
            out.reserveCapacity(count)
            while out.count < count {
                guard let data = try handle.read(upToCount: count - out.count), !data.isEmpty else { break }
                out.append(contentsOf: data)
            }
            return out.count == count ? out : nil
        } catch {
            return nil
        }
    }

    public func write(at offset: Int64, _ bytes: [UInt8]) -> Bool {
        do {
            try handle.seek(toOffset: UInt64(offset))
            try handle.write(contentsOf: Data(bytes))
            return true
        } catch {
            return false
        }
    }

    public func sync() -> Bool {
        do {
            try handle.synchronize()
            return true
        } catch {
            return false
        }
    }
}

public final class SecureDriveLocalStorage: SecureDriveStorage {
    public init() {}

    public func withFile<T>(_ container: String, writable: Bool, _ body: (SecureDriveFile) throws -> T) throws -> T {
        let url = URL(fileURLWithPath: container)
        let handle: FileHandle
        do {
            handle = writable ? try FileHandle(forUpdating: url) : try FileHandle(forReadingFrom: url)
        } catch {
            throw SecureDriveError("container_unavailable")
        }
        defer { try? handle.close() }
        return try body(SecureDriveHandleFile(handle: handle))
    }

    public func createEmpty(_ container: String) throws {
        let fd = open(container, O_WRONLY | O_CREAT | O_EXCL, 0o600)
        guard fd >= 0 else { throw SecureDriveError("folder_unavailable") }
        close(fd)
    }

    public func exists(_ container: String) -> Bool { FileManager.default.fileExists(atPath: container) }

    public func removeCreated(_ container: String) {
        _ = unlink(container)
    }

    public func canonical(_ container: String) -> String {
        URL(fileURLWithPath: container).standardizedFileURL.path
    }

    public func displayName(_ container: String) -> String {
        URL(fileURLWithPath: container).deletingPathExtension().lastPathComponent
    }
}

public enum SecureDriveContent {
    case bytes([UInt8])
    case file(URL)

    func withReader<T>(_ body: (Int64, (Int) throws -> [UInt8]) throws -> T) throws -> T {
        switch self {
        case .bytes(let bytes):
            var position = 0
            return try body(Int64(bytes.count)) { count in
                let end = min(bytes.count, position + count)
                defer { position = end }
                return Array(bytes[position..<end])
            }
        case .file(let url):
            guard let handle = try? FileHandle(forReadingFrom: url) else { throw SecureDriveError("read_failed") }
            defer { try? handle.close() }
            let length = (try? handle.seekToEnd()).map { Int64($0) } ?? 0
            try? handle.seek(toOffset: 0)
            return try body(length) { count in
                var out = [UInt8]()
                while out.count < count {
                    guard let data = try? handle.read(upToCount: count - out.count), !data.isEmpty else { break }
                    out.append(contentsOf: data)
                }
                return out
            }
        }
    }
}
