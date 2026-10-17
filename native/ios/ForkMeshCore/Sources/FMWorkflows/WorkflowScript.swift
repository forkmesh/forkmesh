import FMBase
import Foundation
#if canImport(JavaScriptCore)
import JavaScriptCore
#endif

public protocol WorkflowScript: AnyObject, Sendable {
    func start(_ input: String) -> String
    func resume(_ session: String, _ reply: String) -> String
    func cancel(_ session: String)
    func call(_ name: String, _ arguments: String) -> String
}

public enum WorkflowBundle {
    public static let engineName = "workflow-engine"
    public static let specName = "workflow-spec"

    public static func text(_ name: String, _ ext: String) -> String? {
        let url = Bundle.module.url(forResource: name, withExtension: ext, subdirectory: "Resources")
            ?? Bundle.module.url(forResource: name, withExtension: ext)
        guard let url, let data = try? Data(contentsOf: url) else { return nil }
        return String(decoding: data, as: UTF8.self)
    }

    public static var engineSource: String { text(engineName, "js") ?? "" }
    public static var specSource: String { text(specName, "json") ?? "" }

    public static var spec: JSONObject { JSON.parseObject(specSource) }

    public static var isAvailable: Bool {
        #if canImport(JavaScriptCore)
        return !engineSource.isEmpty && !specSource.isEmpty
        #else
        return false
        #endif
    }
}

public enum WorkflowScriptError: Error, Equatable, Sendable {
    case unavailable
    case failedToLoad(String)
}

#if canImport(JavaScriptCore)
public final class JavaScriptCoreWorkflowScript: WorkflowScript, @unchecked Sendable {
    private let lock = NSLock()
    private let context: JSContext
    private let engine: JSValue
    private var lastException: String = ""

    public init(source: String = WorkflowBundle.engineSource, spec: String = WorkflowBundle.specSource) throws {
        guard !source.isEmpty, !spec.isEmpty, let context = JSContext() else { throw WorkflowScriptError.unavailable }
        self.context = context
        context.name = "ForkMesh Workflows"
        var failure = ""
        context.exceptionHandler = { (_: JSContext?, exception: JSValue?) in
            failure = exception?.toString() ?? "JavaScript error"
        }
        context.evaluateScript(source)
        guard failure.isEmpty, let engine = context.objectForKeyedSubscript("ForkMeshWorkflows"), engine.isObject else {
            throw WorkflowScriptError.failedToLoad(failure.isEmpty ? "ForkMeshWorkflows is missing" : failure)
        }
        engine.invokeMethod("load", withArguments: [spec])
        guard failure.isEmpty else { throw WorkflowScriptError.failedToLoad(failure) }
        self.engine = engine
        context.exceptionHandler = { [weak self] (_: JSContext?, exception: JSValue?) in
            self?.lastException = exception?.toString() ?? "JavaScript error"
        }
    }

    private func invoke(_ method: String, _ arguments: [Any]) -> String {
        lock.lock()
        defer { lock.unlock() }
        lastException = ""
        let value = engine.invokeMethod(method, withArguments: arguments)
        if !lastException.isEmpty {
            return JSON.encode(.object(["done": .object([
                "status": "error", "error": .string(lastException), "requests": 0, "nodes": .object([:]),
            ])]))
        }
        guard let value, value.isString, let text = value.toString() else { return "{}" }
        return text
    }

    public func start(_ input: String) -> String { invoke("start", [input]) }

    public func resume(_ session: String, _ reply: String) -> String { invoke("resume", [session, reply]) }

    public func cancel(_ session: String) { _ = invoke("cancel", [session]) }

    public func call(_ name: String, _ arguments: String) -> String { invoke("call", [name, arguments]) }
}
#endif

public enum WorkflowScripts {
    public static func onDevice() throws -> WorkflowScript {
        #if canImport(JavaScriptCore)
        return try JavaScriptCoreWorkflowScript()
        #else
        throw WorkflowScriptError.unavailable
        #endif
    }
}
