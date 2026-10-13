// The one wrapper around the OS zlib (`import CZlib`). It deflates and
// inflates for the loose object store and the pack reader and writer, and
// reports how many input bytes one zlib stream consumed (pack parsing needs
// the end of each entry).

import CZlib

public enum GitZlibFailure: Error, Equatable, Sendable {
    /// The stream is not valid zlib/deflate data (or has a bad Adler-32).
    case invalidStream
    /// The input ended before the stream did.
    case truncated
    /// The output would pass the caller's bound.
    case outputLimitExceeded
    /// zlib itself failed (allocation, version mismatch).
    case internalFailure
}

public struct GitZlibInflated: Sendable {
    public let output: [UInt8]
    /// Input bytes the stream occupied, Adler-32 trailer included.
    public let consumed: Int
}

public enum GitZlib {
    /// `ZLibEncoder().convert(bytes)`: zlib framing at the default level.
    public static func deflate(_ input: [UInt8], level: Int32 = Z_DEFAULT_COMPRESSION) throws -> [UInt8] {
        var stream = z_stream()
        guard deflateInit_(&stream, level, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw GitZlibFailure.internalFailure
        }
        defer { deflateEnd(&stream) }
        var output = [UInt8]()
        var chunk = [UInt8](repeating: 0, count: 64 * 1024)
        var failure = false
        // zlib counts in 32-bit units; feed the input in slices it can address.
        let slice = 1 << 30
        var offset = 0
        input.withUnsafeBufferPointer { source in
            repeat {
                let count = min(slice, input.count - offset)
                let last = offset + count == input.count
                stream.next_in = count == 0 ? nil : UnsafeMutablePointer(mutating: source.baseAddress! + offset)
                stream.avail_in = UInt32(count)
                var status: Int32 = Z_OK
                repeat {
                    let chunkCount = chunk.count
                    let produced: Int = chunk.withUnsafeMutableBufferPointer { target in
                        stream.next_out = target.baseAddress
                        stream.avail_out = UInt32(chunkCount)
                        status = CZlib.deflate(&stream, last ? Z_FINISH : Z_NO_FLUSH)
                        return chunkCount - Int(stream.avail_out)
                    }
                    if status == Z_STREAM_ERROR { failure = true; return }
                    output.append(contentsOf: chunk[0..<produced])
                } while stream.avail_out == 0 || (last && status != Z_STREAM_END)
                offset += count
            } while offset < input.count
        }
        if failure { throw GitZlibFailure.internalFailure }
        return output
    }

    /// Inflates the single zlib stream that starts at `input[start]` and must
    /// end before `limit`. Output past `maximumOutput` bytes is a failure, not
    /// a truncation, so a small stream can never expand without bound.
    public static func inflate(
        _ input: [UInt8],
        start: Int = 0,
        limit: Int? = nil,
        maximumOutput: Int,
        sizeHint: Int? = nil
    ) throws -> GitZlibInflated {
        let end = limit ?? input.count
        precondition(start >= 0 && start <= end && end <= input.count)
        return try input.withUnsafeBytes { source in
            try inflate(source, start: start, limit: end, maximumOutput: maximumOutput, sizeHint: sizeHint)
        }
    }

    public static func inflate(
        _ source: UnsafeRawBufferPointer,
        start: Int,
        limit end: Int,
        maximumOutput: Int,
        sizeHint: Int? = nil
    ) throws -> GitZlibInflated {
        precondition(start >= 0 && start <= end && end <= source.count)
        var stream = z_stream()
        guard inflateInit_(&stream, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw GitZlibFailure.internalFailure
        }
        defer { inflateEnd(&stream) }
        var output = [UInt8]()
        if let sizeHint, sizeHint > 0 { output.reserveCapacity(min(sizeHint, maximumOutput)) }
        let chunkBytes = max(4096, min(64 * 1024, (sizeHint ?? 64 * 1024) + 1))
        var chunk = [UInt8](repeating: 0, count: chunkBytes)
        var failure: GitZlibFailure?
        var consumed = 0
        let base = source.baseAddress?.assumingMemoryBound(to: UInt8.self)
        var offset = start
        let slice = 1 << 30
        feeding: while true {
            let count = min(slice, end - offset)
            stream.next_in = count == 0 || base == nil ? nil : UnsafeMutablePointer(mutating: base! + offset)
            stream.avail_in = UInt32(count)
            var status: Int32 = Z_OK
            repeat {
                let chunkCount = chunk.count
                let produced: Int = chunk.withUnsafeMutableBufferPointer { target in
                    stream.next_out = target.baseAddress
                    stream.avail_out = UInt32(chunkCount)
                    status = CZlib.inflate(&stream, Z_NO_FLUSH)
                    return chunkCount - Int(stream.avail_out)
                }
                if status == Z_NEED_DICT || status == Z_DATA_ERROR || status == Z_STREAM_ERROR {
                    failure = .invalidStream
                    break feeding
                }
                if status == Z_MEM_ERROR {
                    failure = .internalFailure
                    break feeding
                }
                if output.count + produced > maximumOutput {
                    failure = .outputLimitExceeded
                    break feeding
                }
                output.append(contentsOf: chunk[0..<produced])
                if status == Z_STREAM_END {
                    consumed = offset + count - Int(stream.avail_in) - start
                    break feeding
                }
            } while stream.avail_out == 0
            offset += count
            if offset >= end {
                failure = .truncated
                break feeding
            }
        }
        if let failure { throw failure }
        return GitZlibInflated(output: output, consumed: consumed)
    }
}
