// The zip reading GitWorkspace.importFromZip needs.
//
// Reads the central directory (not the local headers, which a zip may leave
// incomplete), inflates stored (0) and deflated (8) entries with the system
// zlib, and verifies every entry's CRC-32 as `verify: true` did. Encrypted
// entries, other compression methods and ZIP64 archives are refused as not a
// valid zip; a phone import never needs them.

import CZlib
import FMGit
import Foundation

struct LocalZipError: Error, Equatable, CustomStringConvertible {
    let message: String
    init(_ message: String) { self.message = message }
    var description: String { message }
}

struct LocalZipArchive {
    struct Entry: Equatable {
        /// Path inside the archive, `/`-separated, as stored.
        let name: String
        let method: Int
        let flags: Int
        let crc32: UInt32
        let compressedSize: Int
        /// Uncompressed size as declared by the central directory.
        let size: Int
        let localHeaderOffset: Int
        let isDirectory: Bool
        let isSymbolicLink: Bool

        var isFile: Bool { !isDirectory }
    }

    let bytes: [UInt8]
    let entries: [Entry]

    init(bytes: [UInt8]) throws {
        self.bytes = bytes
        self.entries = try LocalZipArchive.readCentralDirectory(bytes)
    }

    /// The uncompressed bytes of `entry`, CRC-checked. More than
    /// `maximumBytes` of output is a resource-limit failure, whatever the
    /// directory declared.
    func content(of entry: Entry, maximumBytes: Int) throws -> [UInt8] {
        if entry.flags & 0x1 != 0 { throw LocalZipError("encrypted entries are not supported") }
        let header = entry.localHeaderOffset
        guard header >= 0, header + 30 <= bytes.count, u32(bytes, header) == 0x0403_4b50 else {
            throw LocalZipError("bad local file header")
        }
        let nameLength = u16(bytes, header + 26)
        let extraLength = u16(bytes, header + 28)
        let start = header + 30 + nameLength + extraLength
        let end = start + entry.compressedSize
        guard start <= bytes.count, end <= bytes.count, end >= start else {
            throw LocalZipError("entry data is truncated")
        }
        let output: [UInt8]
        switch entry.method {
        case 0:
            if entry.compressedSize > maximumBytes {
                throw GitException(.resourceLimitExceeded, "a file exceeds the maximum object size")
            }
            output = Array(bytes[start..<end])
        case 8:
            output = try LocalZipArchive.inflateRaw(bytes, start: start, end: end, maximumOutput: maximumBytes)
        default:
            throw LocalZipError("unsupported compression method \(entry.method)")
        }
        if output.count != entry.size {
            throw LocalZipError("entry size does not match the directory")
        }
        if gitCrc32(output) != entry.crc32 {
            throw LocalZipError("CRC-32 mismatch")
        }
        return output
    }

    // MARK: - Central directory

    static func readCentralDirectory(_ bytes: [UInt8]) throws -> [Entry] {
        guard bytes.count >= 22 else { throw LocalZipError("too short for a zip archive") }
        // End of central directory: the last signature within the maximum
        // comment length of the end.
        var eocd = -1
        var index = bytes.count - 22
        let floor = max(0, bytes.count - 22 - 0xFFFF)
        while index >= floor {
            if u32(bytes, index) == 0x0605_4b50 {
                eocd = index
                break
            }
            index -= 1
        }
        if eocd < 0 { throw LocalZipError("end of central directory not found") }
        let count = u16(bytes, eocd + 10)
        let size = Int(u32(bytes, eocd + 12))
        let offset = Int(u32(bytes, eocd + 16))
        if count == 0xFFFF || size == 0xFFFF_FFFF || offset == 0xFFFF_FFFF {
            throw LocalZipError("ZIP64 archives are not supported")
        }
        guard offset + size <= eocd else { throw LocalZipError("central directory is out of range") }
        var entries: [Entry] = []
        var cursor = offset
        for _ in 0..<count {
            guard cursor + 46 <= bytes.count, u32(bytes, cursor) == 0x0201_4b50 else {
                throw LocalZipError("bad central directory entry")
            }
            let madeBy = u16(bytes, cursor + 4)
            let flags = u16(bytes, cursor + 8)
            let method = u16(bytes, cursor + 10)
            let crc = u32(bytes, cursor + 16)
            let compressed = Int(u32(bytes, cursor + 20))
            let uncompressed = Int(u32(bytes, cursor + 24))
            let nameLength = u16(bytes, cursor + 28)
            let extraLength = u16(bytes, cursor + 30)
            let commentLength = u16(bytes, cursor + 32)
            let external = u32(bytes, cursor + 38)
            let local = Int(u32(bytes, cursor + 42))
            let nameStart = cursor + 46
            guard nameStart + nameLength <= bytes.count else { throw LocalZipError("entry name is truncated") }
            if compressed == 0xFFFF_FFFF || uncompressed == 0xFFFF_FFFF || local == 0xFFFF_FFFF {
                throw LocalZipError("ZIP64 archives are not supported")
            }
            let name = String(decoding: bytes[nameStart..<(nameStart + nameLength)], as: UTF8.self)
            let unixMode = (madeBy >> 8) == 3 ? Int(external >> 16) : 0
            let isSymlink = unixMode & 0o170000 == 0o120000
            let isDirectory = name.utf8.last == 0x2f || external & 0x10 != 0 || unixMode & 0o170000 == 0o040000
            entries.append(Entry(
                name: name, method: method, flags: flags, crc32: crc, compressedSize: compressed,
                size: uncompressed, localHeaderOffset: local, isDirectory: isDirectory, isSymbolicLink: isSymlink))
            cursor = nameStart + nameLength + extraLength + commentLength
        }
        return entries
    }

    // MARK: - Raw deflate

    static func inflateRaw(_ input: [UInt8], start: Int, end: Int, maximumOutput: Int) throws -> [UInt8] {
        var stream = z_stream()
        guard inflateInit2_(&stream, -15, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw LocalZipError("zlib failed to initialise")
        }
        defer { inflateEnd(&stream) }
        var output: [UInt8] = []
        var chunk = [UInt8](repeating: 0, count: 64 * 1024)
        var failure: Error?
        input.withUnsafeBufferPointer { source in
            let count = end - start
            stream.next_in = count == 0 ? nil : UnsafeMutablePointer(mutating: source.baseAddress! + start)
            stream.avail_in = UInt32(count)
            while true {
                var status: Int32 = Z_OK
                let chunkCount = chunk.count
                let produced: Int = chunk.withUnsafeMutableBufferPointer { target in
                    stream.next_out = target.baseAddress
                    stream.avail_out = UInt32(chunkCount)
                    status = CZlib.inflate(&stream, Z_NO_FLUSH)
                    return chunkCount - Int(stream.avail_out)
                }
                if status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR {
                    failure = LocalZipError("invalid deflate stream")
                    return
                }
                if output.count + produced > maximumOutput {
                    failure = GitException(.resourceLimitExceeded, "a file exceeds the maximum object size")
                    return
                }
                output.append(contentsOf: chunk[0..<produced])
                if status == Z_STREAM_END { return }
                if status == Z_BUF_ERROR || (produced == 0 && stream.avail_in == 0) {
                    failure = LocalZipError("deflate stream is truncated")
                    return
                }
            }
        }
        if let failure { throw failure }
        return output
    }

    // MARK: - Little-endian readers

    private static func u16(_ bytes: [UInt8], _ at: Int) -> Int {
        guard at >= 0, at + 2 <= bytes.count else { return 0 }
        return Int(bytes[at]) | Int(bytes[at + 1]) << 8
    }

    private static func u32(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        guard at >= 0, at + 4 <= bytes.count else { return 0 }
        return UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
    }

    private func u16(_ bytes: [UInt8], _ at: Int) -> Int { LocalZipArchive.u16(bytes, at) }
    private func u32(_ bytes: [UInt8], _ at: Int) -> UInt32 { LocalZipArchive.u32(bytes, at) }
}
