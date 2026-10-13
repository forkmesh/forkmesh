/// Smart HTTP service names and their media types.
public struct GitSmartHttpService: Equatable, Sendable {
    public static let uploadPack = GitSmartHttpService(name: "git-upload-pack")
    public static let receivePack = GitSmartHttpService(name: "git-receive-pack")

    public let name: String

    private init(name: String) { self.name = name }

    public var advertisementContentType: String { "application/x-\(name)-advertisement" }
    public var requestContentType: String { "application/x-\(name)-request" }
    public var resultContentType: String { "application/x-\(name)-result" }
}

public enum GitSmartHttp {
    /// Parses a `GET info/refs?service=git-upload-pack` body as a protocol v2
    /// capability advertisement.
    ///
    /// When the request carried `Git-Protocol: version=2`, `git http-backend`
    /// omits the `# service=` banner and starts directly with `version 2`;
    /// older wrappers still prepend the banner and a flush packet. Both forms
    /// are accepted. A v0/v1 ref advertisement fails with
    /// `protocolUnsupported` because its first packet is not the v2 marker.
    public static func parseUploadPackAdvertisement(
        _ body: [UInt8],
        maxPackets: Int = GitResourceLimits.defaultMaxPktLines
    ) throws -> GitProtocolV2Advertisement {
        let packets = try GitPktLine.decodeAll(body, maximumPackets: maxPackets)
        if packets.count >= 2,
           packets[0].kind == .data,
           matchesService(packets[0].payload, GitSmartHttpService.uploadPack.name),
           packets[1].kind == .flush
        {
            return try GitProtocolV2Advertisement.parse(Array(packets[2...]))
        }
        return try GitProtocolV2Advertisement.parse(packets)
    }

    public static func parseReceivePackAdvertisement(
        _ body: [UInt8],
        maxPackets: Int = GitResourceLimits.defaultMaxPktLines,
        maxRefCount: Int = GitResourceLimits.defaultMaxRefCount
    ) throws -> GitReceivePackAdvertisement {
        let packets = try GitPktLine.decodeAll(body, maximumPackets: maxPackets)
        let service = GitSmartHttpService.receivePack
        if packets.count < 3 || packets[0].kind != .data || !matchesService(packets[0].payload, service.name)
            || packets[1].kind != .flush
        {
            throw GitException(.protocolUnsupported, "\(service.name) advertisement banner is invalid")
        }
        return try GitReceivePackAdvertisement.parse(Array(packets[2...]), maxRefCount: maxRefCount)
    }

    private static func matchesService(_ payload: [UInt8], _ service: String) -> Bool {
        payload == Array("# service=\(service)\n".utf8)
    }
}
