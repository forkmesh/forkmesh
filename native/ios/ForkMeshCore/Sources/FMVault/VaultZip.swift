import CZlib
import Foundation

public enum VaultZip {
    public static func isZip(_ data: Data) -> Bool {
        data.count >= 4 && data.prefix(4) == Data([0x50, 0x4b, 0x03, 0x04])
    }

    public static func firstCSV(in data: Data, maximumBytes: Int) throws -> Data {
        let bytes = [UInt8](data)
        guard bytes.count >= 22 else { throw VaultError("zip_unreadable") }
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
        guard eocd >= 0 else { throw VaultError("zip_unreadable") }
        let count = u16(bytes, eocd + 10)
        var cursor = Int(u32(bytes, eocd + 16))
        for _ in 0..<count {
            guard cursor + 46 <= bytes.count, u32(bytes, cursor) == 0x0201_4b50 else { throw VaultError("zip_unreadable") }
            let flags = u16(bytes, cursor + 8)
            let method = u16(bytes, cursor + 10)
            let crc = u32(bytes, cursor + 16)
            let compressed = Int(u32(bytes, cursor + 20))
            let size = Int(u32(bytes, cursor + 24))
            let nameLength = u16(bytes, cursor + 28)
            let extraLength = u16(bytes, cursor + 30)
            let commentLength = u16(bytes, cursor + 32)
            let local = Int(u32(bytes, cursor + 42))
            let nameStart = cursor + 46
            guard nameStart + nameLength <= bytes.count else { throw VaultError("zip_unreadable") }
            let name = String(decoding: bytes[nameStart..<(nameStart + nameLength)], as: UTF8.self)
            cursor = nameStart + nameLength + extraLength + commentLength
            let leaf = name.split(separator: "/").last.map(String.init) ?? ""
            guard name.lowercased().hasSuffix(".csv"), !name.hasPrefix("__MACOSX/"), !leaf.hasPrefix(".") else { continue }
            if flags & 0x1 != 0 { throw VaultError("zip_encrypted") }
            if size > maximumBytes { throw VaultError("csv_too_large") }
            guard local + 30 <= bytes.count, u32(bytes, local) == 0x0403_4b50 else { throw VaultError("zip_unreadable") }
            let start = local + 30 + u16(bytes, local + 26) + u16(bytes, local + 28)
            let end = start + compressed
            guard start <= end, end <= bytes.count else { throw VaultError("zip_unreadable") }
            let output: [UInt8]
            switch method {
            case 0: output = Array(bytes[start..<end])
            case 8: output = try inflateRaw(bytes, start: start, end: end, maximumOutput: maximumBytes)
            default: throw VaultError("zip_unreadable")
            }
            let check = output.withUnsafeBufferPointer { UInt32(CZlib.crc32(0, $0.baseAddress, uInt($0.count))) }
            guard output.count == size, check == crc else { throw VaultError("zip_unreadable") }
            return Data(output)
        }
        throw VaultError("zip_no_csv")
    }

    static func inflateRaw(_ input: [UInt8], start: Int, end: Int, maximumOutput: Int) throws -> [UInt8] {
        var stream = z_stream()
        guard inflateInit2_(&stream, -15, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw VaultError("zip_unreadable")
        }
        defer { inflateEnd(&stream) }
        var output: [UInt8] = []
        var chunk = [UInt8](repeating: 0, count: 64 * 1024)
        var failure: VaultError?
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
                if status != Z_OK && status != Z_STREAM_END {
                    failure = VaultError("zip_unreadable")
                    return
                }
                if output.count + produced > maximumOutput {
                    failure = VaultError("csv_too_large")
                    return
                }
                output.append(contentsOf: chunk[0..<produced])
                if status == Z_STREAM_END { return }
                if produced == 0 && stream.avail_in == 0 {
                    failure = VaultError("zip_unreadable")
                    return
                }
            }
        }
        if let failure { throw failure }
        return output
    }

    private static func u16(_ bytes: [UInt8], _ at: Int) -> Int {
        guard at >= 0, at + 2 <= bytes.count else { return 0 }
        return Int(bytes[at]) | Int(bytes[at + 1]) << 8
    }

    private static func u32(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        guard at >= 0, at + 4 <= bytes.count else { return 0 }
        return UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
    }
}
