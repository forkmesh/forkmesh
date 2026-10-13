// New for the native app (WS7): one host's live log on the phone. The desktop taps the lines its
// own log feeds already receive from the host (the systemd journal, the node log, the web front
// ends: desktop/src/HostLogSources.h) and sends them in numbered batches:
//
//   hosts.logs.start {host}        -> {stream, backlog, window, maxBuffered}
//   event hosts.log {stream, seq, lines:[{t, src, text}], dropped}
//   hosts.logs.ack  {stream, seq}  (this side has taken the batch)
//   hosts.logs.stop {stream}
//
// Backpressure is the desktop's window: it sends at most `window` batches this side has not
// acknowledged, and while the phone lags it keeps only the newest lines and reports how many it
// dropped. This side acknowledges every batch as soon as it is buffered, and caps what it keeps.

import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct HostLogLine: Equatable, Sendable, Identifiable {
    /// Position in this viewer, for list identity.
    public var id: Int
    public var atMs: Int
    public var source: String
    public var text: String

    public init(id: Int, atMs: Int, source: String, text: String) {
        self.id = id
        self.atMs = atMs
        self.source = source
        self.text = text
    }

    public var copyText: String { source.isEmpty ? text : "[\(source)] \(text)" }
}

/// The viewer's buffer, independent of any channel so it can be tested on its own: auto-scroll,
/// pause, search and copy all read from here.
public struct HostLogBuffer: Equatable, Sendable {
    public private(set) var lines: [HostLogLine] = []
    /// Lines that arrived while paused, shown on resume.
    public private(set) var held: [HostLogLine] = []
    public private(set) var paused = false
    /// Lines lost: dropped by the desktop while this side lagged, or trimmed here.
    public private(set) var dropped = 0
    public let limit: Int
    private var nextId = 0

    public init(limit: Int = 5000) {
        self.limit = max(100, limit)
    }

    /// A backlog line as the desktop's detail reports it: "[src] text".
    public mutating func appendBacklog(_ raw: [String]) {
        for line in raw {
            var source = ""
            var text = line
            if line.hasPrefix("["), let close = line.firstIndex(of: "]") {
                source = String(line[line.index(after: line.startIndex)..<close])
                text = String(line[line.index(after: close)...]).trimmingCharacters(in: .whitespaces)
            }
            add(HostLogLine(id: takeId(), atMs: 0, source: source, text: text))
        }
    }

    /// One `hosts.log` batch.
    public mutating func append(batch: JSONObject) {
        dropped += max(0, batch.int("dropped"))
        for line in batch.maps("lines") {
            add(HostLogLine(id: takeId(), atMs: line.int("t"), source: line.str("src"), text: line.str("text")))
        }
    }

    public mutating func setPaused(_ value: Bool) {
        if paused == value { return }
        paused = value
        if !paused {
            lines.append(contentsOf: held)
            held.removeAll()
            trim()
        }
    }

    public mutating func clear() {
        lines.removeAll()
        held.removeAll()
    }

    /// Lines whose text or source contains `query` (case-insensitive); all of them for "".
    public func filtered(_ query: String) -> [HostLogLine] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        if needle.isEmpty { return lines }
        return lines.filter {
            $0.text.localizedCaseInsensitiveContains(needle) || $0.source.localizedCaseInsensitiveContains(needle)
        }
    }

    /// What Copy puts on the clipboard: the lines shown, one per line.
    public func copyText(_ query: String = "") -> String {
        filtered(query).map(\.copyText).joined(separator: "\n")
    }

    private mutating func takeId() -> Int {
        nextId += 1
        return nextId
    }

    private mutating func add(_ line: HostLogLine) {
        if paused {
            held.append(line)
            if held.count > limit {
                dropped += held.count - limit
                held.removeFirst(held.count - limit)
            }
        } else {
            lines.append(line)
            trim()
        }
    }

    private mutating func trim() {
        if lines.count > limit {
            dropped += lines.count - limit
            lines.removeFirst(lines.count - limit)
        }
    }
}

/// A live log for one host.
@MainActor
@Observable
public final class HostLogStream {
    @ObservationIgnored public let channel: HostsChannel
    public let host: String

    public private(set) var buffer = HostLogBuffer()
    public private(set) var live = false
    public private(set) var error = ""
    /// Bumped whenever lines are shown, so a view can scroll to the bottom.
    public private(set) var revision = 0

    @ObservationIgnored private var stream = ""
    @ObservationIgnored private var tapId: UUID?

    public init(channel: HostsChannel, host: String) {
        self.channel = channel
        self.host = host
    }

    public func start() async {
        if live { return }
        if tapId == nil {
            tapId = channel.addEventTap { [weak self] name, data in
                guard let self else { return }
                if name == "hosts.log" { self.onBatch(data) }
                if name == "__closed" { self.onChannelClosed() }
            }
        }
        do {
            let result = try await channel.request("hosts.logs.start", ["host": .string(host)], timeout: 20)
            stream = result.str("stream")
            buffer.appendBacklog(result["backlog"]?.strings ?? [])
            live = !stream.isEmpty
            error = ""
            revision &+= 1
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }

    public func stop() async {
        if let tapId { channel.removeEventTap(tapId) }
        tapId = nil
        let id = stream
        stream = ""
        live = false
        if !id.isEmpty { _ = try? await channel.request("hosts.logs.stop", ["stream": .string(id)], timeout: 10) }
    }

    public func setPaused(_ paused: Bool) {
        buffer.setPaused(paused)
        revision &+= 1
    }

    public func clear() {
        buffer.clear()
        revision &+= 1
    }

    func onBatch(_ data: JSONObject) {
        guard !stream.isEmpty, data.str("stream") == stream else { return }
        buffer.append(batch: data)
        revision &+= 1
        // Taken: open the desktop's window for the next batch. Pausing only holds lines back from
        // the view; acknowledging keeps the stream flowing, bounded by the buffer's own cap.
        let seq = data.int("seq")
        let id = stream
        Task { _ = try? await self.channel.request("hosts.logs.ack", ["stream": .string(id), "seq": .int(Int64(seq))], timeout: 10) }
    }

    private func onChannelClosed() {
        live = false
        stream = ""
        error = "The desktop connection closed. Reopen the log to continue."
    }
}
