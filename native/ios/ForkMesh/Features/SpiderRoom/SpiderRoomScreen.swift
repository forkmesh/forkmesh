import ARKit
import AVFoundation
import RealityKit
import SwiftUI

struct SpiderRoomScreen: View {
    @State private var stage: SpiderRoomStage = SpiderRoomStage()
    @State private var access: AVAuthorizationStatus = AVCaptureDevice.authorizationStatus(for: .video)
    @Environment(\.openURL) private var openURL

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if !ARWorldTrackingConfiguration.isSupported {
                notice("This device can't show augmented reality, so the spider can't come into your room.")
            } else {
                switch access {
                case .authorized:
                    SpiderRoomView(stage: stage).ignoresSafeArea(edges: .bottom)
                    overlay
                case .notDetermined:
                    ProgressView().tint(.white)
                default:
                    VStack(spacing: FmSpace.x3) {
                        notice("The spider needs the camera to see your room. Pictures stay on this phone.")
                        Button("Open Settings") {
                            if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
                        }
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("spider-room-settings")
                    }
                }
            }
        }
        .navigationTitle("Spider Room")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            guard access == .notDetermined else { return }
            let granted: Bool = await AVCaptureDevice.requestAccess(for: .video)
            access = granted ? .authorized : .denied
        }
        .accessibilityIdentifier("spider-room")
    }

    private func notice(_ text: String) -> some View {
        Text(text)
            .multilineTextAlignment(.center)
            .foregroundStyle(.white)
            .padding(.horizontal, FmSpace.x5)
    }

    private var overlay: some View {
        VStack(spacing: FmSpace.x2) {
            if let tracking = stage.tracking {
                chip(tracking, symbol: "camera.viewfinder")
            } else if !stage.floorFound {
                chip("Move your phone slowly around the room so the spider can find the floor, walls and furniture.", symbol: "arkit")
            } else if !stage.found.isEmpty {
                chip("Found \(stage.found).", symbol: "checkmark.circle")
            }
            Spacer()
            Text("Drag the spider to throw it. Tap a spot to send it there, or tap the spider to tickle it.")
                .font(.system(size: 13))
                .multilineTextAlignment(.center)
                .foregroundStyle(.white)
                .shadow(radius: 3)
                .padding(.horizontal, FmSpace.x5)
            HStack(spacing: FmSpace.x3) {
                pill(stage.arrived ? "Call the spider" : "Bring the spider", symbol: "figure.wave") { stage.summon() }
                    .accessibilityIdentifier("spider-room-call")
                if stage.webs > 0 {
                    pill("Clear webs", symbol: "sparkles") { stage.clearWebs() }
                        .accessibilityIdentifier("spider-room-clear-webs")
                }
            }
            .padding(.bottom, FmSpace.x5)
        }
        .padding(.top, FmSpace.x3)
    }

    private func chip(_ text: String, symbol: String) -> some View {
        Label(text, systemImage: symbol)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(.white)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(Capsule().fill(Color.black.opacity(0.55)))
            .padding(.horizontal, FmSpace.x4)
    }

    private func pill(_ title: String, symbol: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: symbol)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 16)
                .frame(minHeight: 44)
                .background(Capsule().fill(Color.black.opacity(0.6)))
        }
        .buttonStyle(.plain)
    }
}

struct SpiderRoomView: UIViewRepresentable {
    let stage: SpiderRoomStage

    func makeCoordinator() -> SpiderRoomDirector { SpiderRoomDirector(stage: stage) }

    func makeUIView(context: Context) -> ARView {
        context.coordinator.start()
        return context.coordinator.arView
    }

    func updateUIView(_ uiView: ARView, context: Context) {}

    static func dismantleUIView(_ uiView: ARView, coordinator: SpiderRoomDirector) {
        coordinator.stop()
    }
}
