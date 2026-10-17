import Foundation
import SwiftData
import NexusCore

@Model
public final class NexusArchive {
    @Attribute(.unique) public var key: String
    public var updatedAt: Date
    public var payload: Data

    public init(key: String, updatedAt: Date = Date(), payload: Data) {
        self.key = key
        self.updatedAt = updatedAt
        self.payload = payload
    }
}

@MainActor
public final class NexusStore {
    public let container: ModelContainer
    private let context: ModelContext
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    public init(container: ModelContainer? = nil, inMemory: Bool = false) throws {
        let configuration = ModelConfiguration(isStoredInMemoryOnly: inMemory)
        self.container = try container ?? ModelContainer(for: NexusArchive.self, configurations: configuration)
        self.context = ModelContext(self.container)
        encoder.dateEncodingStrategy = .iso8601
        decoder.dateDecodingStrategy = .iso8601
    }

    public func saveDevices(_ devices: [Device]) throws { try save(devices, key: "devices") }
    public func saveScan(_ session: ScanSession) throws { try save(session, key: "scan.\(session.id.uuidString)") }
    public func saveDiscovery(_ session: DiscoverySession) throws { try save(session, key: "discovery.\(session.id.uuidString)") }
    public func saveChanges(_ changes: [ObservedChange]) throws { try save(changes, key: "changes") }

    public func devices() throws -> [Device] { try read([Device].self, key: "devices") ?? [] }
    public func scans() throws -> [ScanSession] {
        try archives(prefix: "scan.").compactMap { try? decoder.decode(ScanSession.self, from: $0.payload) }.sorted { $0.startedAt > $1.startedAt }
    }
    public func discoveries() throws -> [DiscoverySession] {
        try archives(prefix: "discovery.").compactMap { try? decoder.decode(DiscoverySession.self, from: $0.payload) }.sorted { $0.startedAt > $1.startedAt }
    }
    public func changes() throws -> [ObservedChange] { try read([ObservedChange].self, key: "changes") ?? [] }

    private func save<Value: Encodable>(_ value: Value, key: String) throws {
        let payload = try encoder.encode(value)
        if let archive = try archives(prefix: key).first(where: { $0.key == key }) {
            archive.payload = payload
            archive.updatedAt = Date()
        } else {
            context.insert(NexusArchive(key: key, payload: payload))
        }
        try context.save()
    }

    private func read<Value: Decodable>(_ type: Value.Type, key: String) throws -> Value? {
        guard let archive = try archives(prefix: key).first(where: { $0.key == key }) else { return nil }
        return try decoder.decode(Value.self, from: archive.payload)
    }

    private func archives(prefix: String) throws -> [NexusArchive] {
        try context.fetch(FetchDescriptor<NexusArchive>()).filter { $0.key.hasPrefix(prefix) }
    }
}

public enum NexusReportExporter {
    public static func json<Scans: Encodable>(_ value: Scans) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return try encoder.encode(value)
    }

    public static func csv(_ session: ScanSession) -> Data {
        let header = "host,address,port,state,latency_ms,service,evidence\n"
        let rows = session.results.sorted { ($0.hostKey, $0.port) < ($1.hostKey, $1.port) }.map { result in
            [result.host, result.address ?? "", String(result.port), result.state.rawValue, result.latencyMilliseconds.map { String(format: "%.2f", $0) } ?? "", result.service?.name ?? "", result.service?.evidence.rawValue ?? ""].map(csvField).joined(separator: ",")
        }
        return Data((header + rows.joined(separator: "\n") + "\n").utf8)
    }

    private static func csvField(_ value: String) -> String { "\"\(value.replacingOccurrences(of: "\"", with: "\"\""))\"" }
}
