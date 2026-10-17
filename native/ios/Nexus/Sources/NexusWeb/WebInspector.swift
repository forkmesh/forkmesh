import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import NexusCore
#if canImport(Security)
import Security
#endif

public protocol WebInspecting: Sendable {
    func inspect(url: URL, timeout: Duration) async throws -> HTTPInspection
}

public struct HTTPInspector: WebInspecting {
    public init() {}

    public func inspect(url: URL, timeout: Duration = .seconds(15)) async throws -> HTTPInspection {
        guard url.scheme == "http" || url.scheme == "https" else { throw ConnectionFailure.unsupported("Use an http or https URL.") }
        let delegate = RedirectRecorder()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = timeout.timeInterval
        configuration.timeoutIntervalForResource = timeout.timeInterval
        let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        var request = URLRequest(url: url)
        request.httpMethod = "HEAD"
        request.setValue("NEXUS/1.0", forHTTPHeaderField: "User-Agent")
        let started = ContinuousClock.now
        let (_, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw ConnectionFailure.system(code: 0, message: "The endpoint did not return an HTTP response.") }
        let headers = http.allHeaderFields.map { HTTPHeader(name: String(describing: $0.key), value: String(describing: $0.value)) }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        let observations = SecurityInsights.http(url: url, status: http.statusCode, headers: headers)
        return HTTPInspection(requestedURL: url, finalURL: http.url ?? url, statusCode: http.statusCode, headers: headers, redirects: delegate.redirects, elapsedMilliseconds: started.duration(to: .now).milliseconds, securityObservations: observations, tls: delegate.tls)
    }
}

final class RedirectRecorder: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    private let state = Locked(([URL](), Optional<TLSObservation>.none))

    var redirects: [URL] { state.current.0 }
    var tls: TLSObservation? { state.current.1 }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        if let url = request.url { state.withLock { $0.0.append(url) } }
        completionHandler(request)
    }

    #if canImport(Security)
    func urlSession(_ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        if challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust, let trust = challenge.protectionSpace.serverTrust {
            let host = challenge.protectionSpace.host
            let chain = (0..<SecTrustGetCertificateCount(trust)).compactMap { index -> CertificateSummary? in
                guard let certificate = SecTrustGetCertificateAtIndex(trust, index) else { return nil }
                let values = SecCertificateCopyValues(certificate, nil, nil) as? [CFString: Any] ?? [:]
                let subject = SecCertificateCopySubjectSummary(certificate).map { String($0) } ?? host
                let before = ((values[kSecOIDX509V1ValidityNotBefore] as? [CFString: Any])?[kSecPropertyKeyValue] as? Date) ?? .distantPast
                let after = ((values[kSecOIDX509V1ValidityNotAfter] as? [CFString: Any])?[kSecPropertyKeyValue] as? Date) ?? .distantFuture
                let data = SecCertificateCopyData(certificate) as Data
                return CertificateSummary(subject: subject, subjectCommonName: subject, issuer: "System-provided", issuerCommonName: nil, serialNumber: "Unavailable", version: 3, notBefore: before, notAfter: after, subjectAlternativeNames: [], publicKeyAlgorithm: "Unavailable", publicKeyBits: nil, signatureAlgorithm: "Unavailable", isCertificateAuthority: false, isSelfIssued: false, sha256Fingerprint: data.base64EncodedString())
            }
            let trusted = SecTrustEvaluateWithError(trust, nil)
            state.withLock { $0.1 = TLSObservation(host: host, port: UInt16(challenge.protectionSpace.port), serverName: host, chain: chain, trust: trusted ? .trusted : .untrusted("System trust evaluation failed")) }
        }
        completionHandler(.performDefaultHandling, nil)
    }
    #endif
}

public enum SecurityInsights {
    public static func http(url: URL, status: Int, headers: [HTTPHeader]) -> [SecurityObservation] {
        let table = Dictionary(headers.map { ($0.name.lowercased(), $0.value) }, uniquingKeysWith: { first, _ in first })
        var findings: [SecurityObservation] = []
        func add(_ severity: SecuritySeverity, _ title: String, _ detail: String) { findings.append(SecurityObservation(severity: severity, title: title, detail: detail, subject: url.host ?? url.absoluteString)) }
        if url.scheme == "http" { add(.attention, "Unencrypted HTTP", "This endpoint was inspected over HTTP, so request and response content can be observed or altered on the network.") }
        if table["strict-transport-security"] == nil, url.scheme == "https" { add(.informational, "No HSTS header observed", "The response did not include Strict-Transport-Security.") }
        if table["content-security-policy"] == nil { add(.informational, "No Content-Security-Policy observed", "The response did not include a Content-Security-Policy header.") }
        if table["x-content-type-options"]?.lowercased() != "nosniff" { add(.informational, "MIME sniffing protection not observed", "The response did not include X-Content-Type-Options: nosniff.") }
        if status >= 500 { add(.attention, "Server error response", "The endpoint returned HTTP \(status).") }
        return findings
    }

    public static func portChanges(current: ScanSession, previous: ScanSession?) -> [SecurityObservation] {
        guard let previous else { return [] }
        let oldOpen = Set(previous.openResults.map(\.id)); let newOpen = Set(current.openResults.map(\.id))
        return newOpen.subtracting(oldOpen).sorted().map { SecurityObservation(severity: .attention, title: "New exposed TCP port", detail: "\($0) was not open in the previous scan.", subject: current.target) }
    }
}
