import Foundation
import FMBase

/// Append-only on-disk record of every stall the app notices, in every build.
///
/// The in-app performance HUD only exists in debug builds and forgets everything on restart, so a
/// stall hit on a phone nobody was watching left no trace. This is the mobile counterpart of the
/// desktop node's `~/.forkmesh/diagnostics/stalls.log`: one plain-text record per stall, written
/// off the main thread on a serial queue.
///
/// On iOS it lands in `Library/Application Support/diagnostics/stalls.log` inside the app's data
/// container (`xcrun simctl get_app_container booted com.forkmesh.forkmesh data` on a simulator).
/// Every record is also sent to the log by `PerformanceMonitorService`.
public final class StallLogFile: @unchecked Sendable {
    public static let fileName = "stalls.log"

    /// Past this size the log rotates to `stalls.log.1`, so a janky screen left open for a day
    /// cannot grow app storage without bound.
    public let maxBytes: Int

    private let directory: @Sendable () throws -> URL
    private let clock: Clock
    private let build: AppBuild
    private let timeZone: TimeZone

    // Every file operation runs here, in order.
    private let queue = DispatchQueue(label: "com.forkmesh.stall-log", qos: .utility)
    private let lock = NSLock()
    private var file: URL?
    private var unavailable = false
    private var bytes = 0

    /// `directory` is the root the `diagnostics` folder is created in; it may throw, and is asked
    /// once, on the first record.
    public init(
        directory: @escaping @Sendable () throws -> URL,
        maxBytes: Int = 1024 * 1024,
        build: AppBuild = AppBuild(version: "0.0.0"),
        clock: Clock = SystemClock(),
        timeZone: TimeZone = .current
    ) {
        self.directory = directory
        self.maxBytes = maxBytes
        self.build = build
        self.clock = clock
        self.timeZone = timeZone
    }

    /// The log in this device's Application Support directory (see the class comment).
    public static func forDevice(build: AppBuild) -> StallLogFile {
        StallLogFile(
            directory: {
                try FileManager.default.url(
                    for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            },
            build: build
        )
    }

    /// Absolute path of the log once the first record has opened it.
    public var path: String? {
        lock.lock()
        defer { lock.unlock() }
        return file?.path
    }

    /// Queue `record` for the file. Never throws and never blocks: a device where storage cannot
    /// be resolved simply keeps its records in the platform log.
    public func append(_ record: String) {
        queue.async { [self] in write(record) }
    }

    /// Returns once every queued record has been written (tests, shutdown).
    public func flush() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            queue.async { continuation.resume() }
        }
    }

    /// `flush` for callers that cannot suspend (process teardown). Never call it on the main
    /// thread of a running app.
    public func flushBlocking() {
        queue.sync {}
    }

    public func tail(maxBytes: Int) async -> String {
        await withCheckedContinuation { (continuation: CheckedContinuation<String, Never>) in
            queue.async { [self] in continuation.resume(returning: readTail(maxBytes)) }
        }
    }

    private func readTail(_ maxBytes: Int) -> String {
        guard maxBytes > 0, let root = try? directory() else { return "" }
        let url = root.appendingPathComponent("diagnostics", isDirectory: true).appendingPathComponent(StallLogFile.fileName)
        var data = Data()
        for path in [url.path + ".1", url.path] {
            if let chunk = FileManager.default.contents(atPath: path) { data.append(chunk) }
        }
        if data.count <= maxBytes { return String(decoding: data, as: UTF8.self) }
        let cut = data.suffix(maxBytes)
        let start = cut.firstIndex(of: UInt8(ascii: "\n")).map { cut.index(after: $0) } ?? cut.startIndex
        return String(decoding: cut[start...], as: UTF8.self)
    }

    private func write(_ record: String) {
        guard let file = open() else { return }
        let data = Data((record.hasSuffix("\n") ? record : record + "\n").utf8)
        if bytes + data.count > maxBytes && bytes > 0 {
            // rename(2) replaces an older `.1`. A failed rotation
            // just means the log keeps growing this session.
            _ = rename(file.path, file.path + ".1")
            bytes = 0
            writeBytes(file, Data(sessionHeader("rotated").utf8))
        }
        writeBytes(file, data)
    }

    private func writeBytes(_ file: URL, _ data: Data) {
        if !FileManager.default.fileExists(atPath: file.path) {
            _ = FileManager.default.createFile(atPath: file.path, contents: nil)
        }
        guard let handle = try? FileHandle(forWritingTo: file) else { return }
        defer { try? handle.close() }
        do {
            try handle.seekToEnd()
            try handle.write(contentsOf: data)
            try handle.synchronize()
            bytes += data.count
        } catch {
            // Storage filled up or went away; the record is still in the platform log.
        }
    }

    private func open() -> URL? {
        lock.lock()
        let known = file
        let dead = unavailable
        lock.unlock()
        if let known { return known }
        if dead { return nil }
        do {
            let root = try directory()
            let dir = root.appendingPathComponent("diagnostics", isDirectory: true)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let url = dir.appendingPathComponent(StallLogFile.fileName)
            let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
            bytes = (attributes?[.size] as? NSNumber)?.intValue ?? 0
            lock.lock()
            file = url
            lock.unlock()
            writeBytes(url, Data(sessionHeader("session start").utf8))
            return url
        } catch {
            // No storage: stalls still reach the log.
            lock.lock()
            unavailable = true
            lock.unlock()
            return nil
        }
    }

    private func sessionHeader(_ label: String) -> String {
        "--- \(label) \(DartDateFormat.iso8601(clock.now, timeZone: timeZone)) build=\(build.version) "
            + "mode=\(StallLogFile.buildMode) platform=\(build.platform) "
            + "os=\"\(build.osVersion)\" ---\n"
    }

    /// "debug" or "release"; a Swift build has no profile mode.
    static var buildMode: String {
        #if DEBUG
        return "debug"
        #else
        return "release"
        #endif
    }
}

/// ISO 8601 for a local time: `yyyy-MM-ddTHH:mm:ss.mmm`, with a trailing `Z` only for UTC.
/// `Date` is not trustworthy past the millisecond this far from its epoch, so this always stops at
/// milliseconds.
public enum DartDateFormat {
    public static func iso8601(_ date: Date, timeZone: TimeZone = .current, utc: Bool = false) -> String {
        var calendar = Calendar(identifier: .gregorian)
        let zone = utc ? (TimeZone(identifier: "UTC") ?? timeZone) : timeZone
        calendar.timeZone = zone
        // Split on whole milliseconds first so 12:00:00.9996 cannot print as ".1000".
        let millis = date.epochMillis
        let whole = Date(timeIntervalSince1970: Double(floorDiv(millis, 1_000)))
        let ms = Int(floorMod(millis, 1_000))
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second], from: whole)
        let text = String(
            format: "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
            c.year ?? 0, c.month ?? 0, c.day ?? 0, c.hour ?? 0, c.minute ?? 0, c.second ?? 0, ms)
        return utc ? text + "Z" : text
    }

    private static func floorDiv(_ a: Int64, _ b: Int64) -> Int64 {
        let q = a / b
        return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q
    }

    private static func floorMod(_ a: Int64, _ b: Int64) -> Int64 {
        let r = a % b
        return r < 0 ? r + b : r
    }
}
