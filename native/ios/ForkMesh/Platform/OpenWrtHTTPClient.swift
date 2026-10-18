import FMNet
import FMPackages
import Foundation
import Security

final class OpenWrtHTTPClient: NSObject, HTTPClient, URLSessionTaskDelegate, @unchecked Sendable {
    private let trusted: String
    private let lock = NSLock()
    private var untrusted = ""
    private var session: URLSession!

    init(trusted: String) {
        self.trusted = trusted
        super.init()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 30
        configuration.timeoutIntervalForResource = 330
        configuration.httpCookieStorage = nil
        configuration.urlCache = nil
        configuration.waitsForConnectivity = false
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
    }

    func invalidate() {
        session.invalidateAndCancel()
    }

    func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        var urlRequest = URLRequest(url: request.url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: request.timeout)
        urlRequest.httpMethod = request.method
        for field in request.headers { urlRequest.setValue(field.value, forHTTPHeaderField: field.name) }
        urlRequest.httpBody = request.body
        do {
            let (data, response) = try await session.data(for: urlRequest)
            guard let http = response as? HTTPURLResponse else { throw HTTPError.invalidResponse }
            var headers = HTTPHeaders()
            for (name, value) in http.allHeaderFields { headers.add("\(name)", "\(value)") }
            return HTTPResponse(status: http.statusCode, headers: headers, body: data)
        } catch let error as HTTPError {
            throw error
        } catch {
            let fingerprint = lock.withLock { untrusted }
            if !fingerprint.isEmpty { throw OpenWrtCertificateError(fingerprint: fingerprint) }
            let code = (error as NSError).code
            if code == NSURLErrorTimedOut { throw HTTPError.timedOut }
            if code == NSURLErrorCancelled { throw HTTPError.cancelled }
            throw HTTPError.transport(error.localizedDescription)
        }
    }

    func urlSession(
        _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest
    ) async -> URLRequest? {
        nil
    }

    func urlSession(
        _ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge
    ) async -> (URLSession.AuthChallengeDisposition, URLCredential?) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust else {
            return (.performDefaultHandling, nil)
        }
        guard let chain = SecTrustCopyCertificateChain(trust) as? [SecCertificate], let leaf = chain.first else {
            return (.cancelAuthenticationChallenge, nil)
        }
        let der = SecCertificateCopyData(leaf) as Data
        let fingerprint = OpenWrtModel.certificateFingerprint([UInt8](der))
        if !trusted.isEmpty && fingerprint == trusted {
            return (.useCredential, URLCredential(trust: trust))
        }
        lock.withLock { untrusted = fingerprint }
        return (.cancelAuthenticationChallenge, nil)
    }

    static func resolve(_ host: String) -> [String] {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var list: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &list) == 0, let first = list else { return [] }
        defer { freeaddrinfo(first) }
        var addresses: [String] = []
        var cursor: UnsafeMutablePointer<addrinfo>? = first
        while let info = cursor {
            var buffer = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(info.pointee.ai_addr, info.pointee.ai_addrlen, &buffer, socklen_t(buffer.count), nil, 0, NI_NUMERICHOST) == 0 {
                let text = String(cString: buffer)
                let bare = String(text.split(separator: "%").first ?? Substring(text))
                if !addresses.contains(bare) { addresses.append(bare) }
            }
            cursor = info.pointee.ai_next
        }
        return addresses
    }
}
