import FMBase
import FMNet
import Foundation

public struct OpenWrtCertificateError: Error, Equatable, Sendable {
    public let fingerprint: String
    public init(fingerprint: String) { self.fingerprint = fingerprint }
}

public actor OpenWrtClient {
    private let http: HTTPClient
    public private(set) var base: URL?
    public private(set) var user: String = ""
    private var session: String = ""
    private var password: String = ""
    private var nextId: Int = 1
    private var renewal: Task<OpenWrtResult, Never>?
    public private(set) var renewals: Int = 0

    public init(http: HTTPClient, base: URL? = nil) {
        self.http = http
        self.base = base
    }

    public var signedIn: Bool { !session.isEmpty }

    public func setBase(_ url: URL) {
        if url != base {
            session = ""
            password = ""
        }
        base = url
    }

    public func rememberPassword(_ value: String) { password = value }

    public func signIn(user: String, password: String) async -> OpenWrtResult {
        session = ""
        self.password = ""
        self.user = user
        var result = await login(user: user, password: password)
        if result.ok {
            session = result.data.str("ubus_rpc_session")
            self.password = password
            if session.isEmpty {
                result.ok = false
                result.error = "The router accepted the sign-in but returned no session."
            }
        } else if result.accessDenied {
            result.error = "The user name or password is wrong."
        }
        return result
    }

    public func signOut() async {
        if !session.isEmpty {
            _ = await send(object: "session", method: "destroy", args: JSONObject(), timeout: 5, session: session)
        }
        session = ""
        password = ""
    }

    public func call(_ object: String, _ method: String, _ args: JSONObject = JSONObject(), timeout: TimeInterval = 20) async -> OpenWrtResult {
        let result = await send(object: object, method: method, args: args, timeout: timeout, session: session)
        guard result.accessDenied, !password.isEmpty else { return result }
        let renewed = await renew()
        guard renewed.ok else {
            var failed = renewed
            failed.ok = false
            failed.accessDenied = true
            failed.error = "The router session ended and signing in again failed: \(renewed.error)"
            return failed
        }
        return await send(object: object, method: method, args: args, timeout: timeout, session: session)
    }

    public func exec(_ command: String, _ params: [String], timeout: TimeInterval = 120) async -> OpenWrtResult {
        var result = await call("file", "exec", ["command": .string(command), "params": JSONValue(params)], timeout: timeout)
        if result.ok {
            let code = result.data.int("code")
            if code != 0 {
                result.ok = false
                let errors = result.data.str("stderr").trimmingCharacters(in: .whitespacesAndNewlines)
                result.error = errors.isEmpty ? "The command exited with code \(code)." : String(errors.suffix(600))
            }
        }
        return result
    }

    private func renew() async -> OpenWrtResult {
        if let renewal { return await renewal.value }
        let user = self.user
        let password = self.password
        let task = Task { await self.login(user: user, password: password) }
        renewal = task
        let result = await task.value
        renewal = nil
        let fresh = result.data.str("ubus_rpc_session")
        if result.ok && !fresh.isEmpty {
            session = fresh
            renewals += 1
            return result
        }
        var failed = result
        failed.ok = false
        return failed
    }

    private func login(user: String, password: String) async -> OpenWrtResult {
        await send(object: "session", method: "login",
                   args: ["username": .string(user), "password": .string(password), "timeout": .int(Int64(OpenWrt.sessionTimeout))],
                   timeout: 20, session: OpenWrt.anonymousSession)
    }

    private func send(object: String, method: String, args: JSONObject, timeout: TimeInterval, session: String) async -> OpenWrtResult {
        guard let base, let url = URL(string: "/ubus", relativeTo: base)?.absoluteURL else {
            return .failure("Enter the router's address first.")
        }
        let id = nextId
        nextId += 1
        let request = HTTPRequest(
            method: "POST", url: url, headers: ["Content-Type": "application/json"],
            body: OpenWrtModel.callBody(id: id, session: session, object: object, method: method, args: args),
            timeout: timeout, followRedirects: false, maxResponseBytes: 16 * 1024 * 1024)
        do {
            let response = try await http.send(request)
            if (301...308).contains(response.status) {
                let location = response.header("Location") ?? ""
                return .failure(location.lowercased().hasPrefix("https:")
                    ? "The router only answers over HTTPS. Change the address to https://\(base.host ?? "")."
                    : "The router redirected the request to \(location). Use that address instead.")
            }
            return OpenWrtModel.parseReply(response.body, httpStatus: response.status)
        } catch let error as OpenWrtCertificateError {
            var result = OpenWrtResult.failure("The router's HTTPS certificate is not trusted yet. Compare its fingerprint with the one the router shows, then choose Trust.")
            result.certificate = error.fingerprint
            return result
        } catch {
            var result = OpenWrtResult.failure("Could not reach the router at \(base.host ?? ""): \(error)")
            result.unreachable = true
            return result
        }
    }
}
