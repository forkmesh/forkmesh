import AppIntents
import FMApi
import FMSurfaces
import Foundation

enum SurfaceIntentProcess {
    static var isExtension: Bool { Bundle.main.bundleURL.pathExtension == "appex" }

    static var openWait: Double { isExtension ? 0 : 4 }

    static var actionWait: Double { isExtension ? 0 : 8 }
}

struct OpenSurfaceRouteIntent: AppIntent {
    static let title: LocalizedStringResource = "Open in ForkMesh"
    static let openAppWhenRun: Bool = true
    static let isDiscoverable: Bool = false

    @Parameter(title: "Destination")
    var token: String

    init() {}

    init(route: Route) {
        self.token = SurfaceRouteCodec.encode(route) ?? ""
    }

    @MainActor
    func perform() async throws -> some IntentResult {
        guard let route = SurfaceRouteCodec.decode(token) else { return .result() }
        try await SurfaceCommandCenter.shared.perform(.open(route), waitSeconds: SurfaceIntentProcess.openWait)
        return .result()
    }
}

struct StopAgentRunIntent: LiveActivityIntent {
    static let title: LocalizedStringResource = "Stop agent run"
    static let isDiscoverable: Bool = false

    @Parameter(title: "Run")
    var runId: String

    init() {}

    init(runId: String) {
        self.runId = runId
    }

    @MainActor
    func perform() async throws -> some IntentResult {
        try await SurfaceCommandCenter.shared.perform(
            .stopAgent(runId: runId), waitSeconds: SurfaceIntentProcess.actionWait)
        return .result()
    }
}

struct ApproveAgentRequestIntent: LiveActivityIntent {
    static let title: LocalizedStringResource = "Approve agent request"
    static let isDiscoverable: Bool = false
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @Parameter(title: "Request")
    var approvalId: String

    init() {}

    init(approvalId: String) {
        self.approvalId = approvalId
    }

    @MainActor
    func perform() async throws -> some IntentResult {
        try await SurfaceCommandCenter.shared.perform(
            .approve(approvalId: approvalId), waitSeconds: SurfaceIntentProcess.actionWait)
        return .result()
    }
}
