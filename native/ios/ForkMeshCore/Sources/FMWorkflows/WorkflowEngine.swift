import FMBase
import FMNet
import Foundation

public struct WorkflowRequest: Equatable, Sendable {
    public var method: String
    public var url: String
    public var headers: JSONObject
    public var body: String?

    public init(method: String, url: String, headers: JSONObject = [:], body: String? = nil) {
        self.method = method
        self.url = url
        self.headers = headers
        self.body = body
    }

    public init(json: JSONObject) {
        let body = json["body"]
        self.init(method: json.str("method", "GET"), url: json.str("url"), headers: json.obj("headers"),
                  body: body?.isNull == false ? body?.stringValue : nil)
    }
}

public struct WorkflowReply: Equatable, Sendable {
    public var status: Int
    public var text: String

    public init(status: Int, text: String) {
        self.status = status
        self.text = text
    }

    public static let noResponse = WorkflowReply(status: 0, text: "")

    public var json: JSONValue { .object(["status": .int(Int64(status)), "text": .string(text)]) }
}

public protocol WorkflowTransport: Sendable {
    func send(_ request: WorkflowRequest, allowed: @escaping @Sendable (String) -> Bool) async -> WorkflowReply
}

public final class WorkflowEngine: @unchecked Sendable {
    public static let maxRedirects = 5

    private let script: WorkflowScript
    public let spec: JSONObject

    public init(script: WorkflowScript, spec: JSONObject = WorkflowBundle.spec) {
        self.script = script
        self.spec = spec
    }

    public static func onDevice() throws -> WorkflowEngine {
        WorkflowEngine(script: try WorkflowScripts.onDevice())
    }

    public var version: String { spec.str("version") }
    public var userAgent: String { spec.str("userAgent") }
    public var requestTimeout: TimeInterval { TimeInterval(spec.obj("limits").int("requestTimeoutSeconds", 30)) }
    public var maxResponseChars: Int { spec.obj("limits").int("maxResponseChars", 262_144) }

    func call(_ name: String, _ arguments: [JSONValue]) -> JSONValue {
        let answer = JSON.parseObject(script.call(name, JSON.encode(.array(arguments))))
        return answer["result"] ?? .null
    }

    public func catalog() -> WorkflowCatalog { WorkflowCatalog(json: call("catalog", []).objectValue ?? [:]) }

    public func clean(_ workflow: JSONObject) -> (workflow: JSONObject?, error: String) {
        let answer = call("clean", [.object(workflow)]).objectValue ?? [:]
        let error = answer.str("error")
        return (error.isEmpty ? answer["workflow"]?.objectValue : nil, error.isEmpty && answer["workflow"] == nil ? "invalid_workflow" : error)
    }

    public func isSafe(_ url: String) -> Bool { !(call("safeUrl", [.string(url)]).stringValue ?? "").isEmpty }

    public func scheduleMinutes(_ workflow: JSONObject) -> Int { call("scheduleMinutes", [.object(workflow)]).int(or: 0) }

    public var maxWorkflowDepth: Int { spec.obj("limits").int("maxWorkflowDepth", 3) }

    public func run(
        _ workflow: JSONObject, payload: JSONValue? = nil, credentials: JSONObject = [:], trigger: String = "manual",
        workflowId: String = "", nowMs: Int64? = nil, workflows: [JSONValue] = [], randoms: [JSONValue]? = nil,
        transport: WorkflowTransport,
        wait: @escaping @Sendable (Int64) async -> Void = { (ms: Int64) in await WorkflowEngine.sleep(ms) }
    ) async -> JSONObject {
        var input: JSONObject = [
            "workflow": .object(workflow), "payload": payload ?? .null, "credentials": .object(credentials),
            "trigger": .string(trigger), "workflowId": .string(workflowId), "workflows": .array(workflows),
        ]
        if let nowMs { input["nowMs"] = .int(nowMs) }
        if let randoms { input["randoms"] = .array(randoms) }
        var step = JSON.parseObject(script.start(JSON.encode(input)))
        while step["request"]?.objectValue != nil || step["wait"] != nil {
            let session = step.str("session")
            if Task.isCancelled {
                script.cancel(session)
                return ["status": "error", "error": "Stopped", "requests": 0, "nodes": .object([:]), "trigger": .string(trigger)]
            }
            if let request = step["request"]?.objectValue {
                let reply = await transport.send(WorkflowRequest(json: request), allowed: { [weak self] (url: String) in
                    self?.isSafe(url) ?? false
                })
                step = JSON.parseObject(script.resume(session, JSON.encode(reply.json)))
            } else {
                await wait(Int64(step.int("wait")))
                step = JSON.parseObject(script.resume(session, "null"))
            }
        }
        return step.obj("done")
    }

    public static func sleep(_ ms: Int64) async {
        try? await Task.sleep(nanoseconds: UInt64(max(0, ms)) * 1_000_000)
    }
}

public final class HTTPWorkflowTransport: WorkflowTransport, @unchecked Sendable {
    private let client: HTTPClient
    private let userAgent: String
    private let timeout: TimeInterval
    private let maxBytes: Int

    public init(client: HTTPClient, userAgent: String, timeout: TimeInterval = 30, maxChars: Int = 262_144) {
        self.client = client
        self.userAgent = userAgent
        self.timeout = timeout
        maxBytes = maxChars * 4
    }

    public convenience init(client: HTTPClient, engine: WorkflowEngine) {
        self.init(client: client, userAgent: engine.userAgent, timeout: engine.requestTimeout, maxChars: engine.maxResponseChars)
    }

    public func send(_ request: WorkflowRequest, allowed: @escaping @Sendable (String) -> Bool) async -> WorkflowReply {
        var method = request.method
        var address = request.url
        var body = request.body.map { Data($0.utf8) }
        var headers = HTTPHeaders()
        for (name, value) in request.headers { headers[name] = value.text }
        if headers["User-Agent"] == nil { headers["User-Agent"] = userAgent }
        for _ in 0...WorkflowEngine.maxRedirects {
            guard allowed(address), let url = URL(string: address) else { return .noResponse }
            let outgoing = HTTPRequest(method: method, url: url, headers: headers, body: body, timeout: timeout,
                                       followRedirects: false, maxResponseBytes: maxBytes)
            let response: HTTPStreamedResponse
            do {
                response = try await client.stream(outgoing)
            } catch {
                return .noResponse
            }
            if [301, 302, 303, 307, 308].contains(response.status), let location = response.headers["Location"],
               let next = URL(string: location, relativeTo: url)?.absoluteURL {
                if response.status == 303 || ([301, 302].contains(response.status) && method == "POST") {
                    method = "GET"
                    body = nil
                    headers["Content-Type"] = nil
                }
                address = next.absoluteString
                continue
            }
            var data = Data()
            do {
                for try await chunk in response.body {
                    data.append(chunk)
                    if data.count >= maxBytes { break }
                }
            } catch {
                if data.isEmpty { return WorkflowReply(status: response.status, text: "") }
            }
            return WorkflowReply(status: response.status, text: Utf8.decodeLossy(data.prefix(maxBytes)))
        }
        return .noResponse
    }
}
