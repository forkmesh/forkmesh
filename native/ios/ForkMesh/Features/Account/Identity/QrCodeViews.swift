import AVFoundation
import CoreImage
import CoreImage.CIFilterBuiltins
import SwiftUI
import UIKit

struct QrCodeImage: View {
    let text: String
    var size: CGFloat = 200

    var body: some View {
        if let image = QrCodeImage.render(text) {
            Image(uiImage: image)
                .interpolation(.none)
                .resizable()
                .scaledToFit()
                .frame(width: size, height: size)
                .padding(12)
                .background(Color.white)
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .accessibilityLabel(Text("QR code"))
        } else {
            EmptyView()
        }
    }

    static func render(_ text: String) -> UIImage? {
        guard !text.isEmpty, text.utf8.count <= 2_000 else { return nil }
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(text.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage else { return nil }
        let scaled = output.transformed(by: CGAffineTransform(scaleX: 8, y: 8))
        let context = CIContext()
        guard let cgImage = context.createCGImage(scaled, from: scaled.extent) else { return nil }
        return UIImage(cgImage: cgImage)
    }
}

struct QrScannerSheet: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.dismiss) var dismiss
    let title: String
    let onScanned: (String) -> Void
    @State private var denied: Bool = false

    var body: some View {
        NavigationStack {
            ZStack {
                if denied {
                    FmEmptyState(
                        icon: "qr_code_scanner_outlined", title: "Camera is off",
                        message: "Allow camera access for ForkMesh in the Settings app, or paste the link instead.")
                } else {
                    QrScannerRepresentable(onScanned: { (value: String) in
                        onScanned(value)
                        dismiss()
                    }, onDenied: { denied = true })
                    .ignoresSafeArea()
                }
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }
}

private struct QrScannerRepresentable: UIViewControllerRepresentable {
    let onScanned: (String) -> Void
    let onDenied: () -> Void

    func makeUIViewController(context: Context) -> QrScannerController {
        let controller = QrScannerController()
        controller.onScanned = onScanned
        controller.onDenied = onDenied
        return controller
    }

    func updateUIViewController(_ controller: QrScannerController, context: Context) {}
}

final class QrScannerController: UIViewController, AVCaptureMetadataOutputObjectsDelegate {
    var onScanned: ((String) -> Void)?
    var onDenied: (() -> Void)?
    private let session = AVCaptureSession()
    private var preview: AVCaptureVideoPreviewLayer?
    private var delivered = false

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            configure()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] (granted: Bool) in
                DispatchQueue.main.async {
                    if granted {
                        self?.configure()
                    } else {
                        self?.onDenied?()
                    }
                }
            }
        default:
            onDenied?()
        }
    }

    private func configure() {
        guard let device = AVCaptureDevice.default(for: .video),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input)
        else {
            onDenied?()
            return
        }
        session.addInput(input)
        let output = AVCaptureMetadataOutput()
        guard session.canAddOutput(output) else {
            onDenied?()
            return
        }
        session.addOutput(output)
        output.setMetadataObjectsDelegate(self, queue: .main)
        output.metadataObjectTypes = [.qr]
        let layer = AVCaptureVideoPreviewLayer(session: session)
        layer.videoGravity = .resizeAspectFill
        layer.frame = view.bounds
        view.layer.addSublayer(layer)
        preview = layer
        let running = session
        DispatchQueue.global(qos: .userInitiated).async {
            running.startRunning()
        }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        preview?.frame = view.bounds
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        let running = session
        DispatchQueue.global(qos: .userInitiated).async {
            if running.isRunning { running.stopRunning() }
        }
    }

    nonisolated func metadataOutput(
        _ output: AVCaptureMetadataOutput, didOutput metadataObjects: [AVMetadataObject], from connection: AVCaptureConnection
    ) {
        guard let code = metadataObjects.compactMap({ $0 as? AVMetadataMachineReadableCodeObject }).first,
              let value = code.stringValue, !value.isEmpty, value.utf8.count <= 2_048
        else { return }
        MainActor.assumeIsolated {
            deliver(value)
        }
    }

    private func deliver(_ value: String) {
        guard !delivered else { return }
        delivered = true
        UINotificationFeedbackGenerator().notificationOccurred(.success)
        onScanned?(value)
    }
}
