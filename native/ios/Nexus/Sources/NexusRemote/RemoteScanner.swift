import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import NexusCore

public struct RemoteScannerConfiguration: Codable, Hashable, Sendable {
    public var baseURL: URL
    public var token: String
    public var agentName: String
    public var allowedProfiles: [String]

    public init(baseURL: URL, token: String, agentName: String, allowedProfiles: [String]) {
        self.baseURL = baseURL
        self.token = token
        self.agentName = agentName
        self.allowedProfiles = allowedProfiles
    }
}

public struct RemoteScanRequest: Codable, Hashable, Sendable {
    public var target: String
    public var ports: PortList
    public var profile: String
    public var attestation: AuthorizationAttestation

    public init(target: String, ports: PortList, profile: String, attestation: AuthorizationAttestation) {
        self.target = target
        self.ports = ports
        self.profile = profile
        self.attestation = attestation
    }
}

public protocol RemoteScanning: Sendable {
    func submit(_ request: RemoteScanRequest) async throws -> ScanSession
}

public struct RemoteScannerClient: RemoteScanning {
    public let configuration: RemoteScannerConfiguration
    public let session: URLSession

    public init(configuration: RemoteScannerConfiguration, session: URLSession = .shared) {
        self.configuration = configuration
        self.session = session
    }

    public func submit(_ request: RemoteScanRequest) async throws -> ScanSession {
        guard configuration.allowedProfiles.contains(request.profile) else { throw ConnectionFailure.unsupported("The remote profile \(request.profile) is not approved by this integration.") }
        var urlRequest = URLRequest(url: configuration.baseURL.appending(path: "v1/scans"))
        urlRequest.httpMethod = "POST"
        urlRequest.setValue("Bearer \(configuration.token)", forHTTPHeaderField: "Authorization")
        urlRequest.setValue("application/json", forHTTPHeaderField: "Content-Type")
        urlRequest.httpBody = try JSONEncoder().encode(request)
        let (data, response) = try await session.data(for: urlRequest)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else { throw ConnectionFailure.system(code: 0, message: "Remote scanner rejected the request.") }
        var scan = try JSONDecoder().decode(ScanSession.self, from: data)
        scan.engine = .remote(agent: configuration.agentName, profile: request.profile)
        return scan
    }
}
