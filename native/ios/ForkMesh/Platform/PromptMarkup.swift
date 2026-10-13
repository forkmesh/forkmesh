import FMAgents
import FMApi
import PencilKit
import SwiftUI
import UIKit

enum PromptScreenCapture {
    @MainActor
    static func window() -> UIWindow? {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        guard let window = scene?.keyWindow ?? scene?.windows.first, window.bounds.width > 0, window.bounds.height > 0 else {
            return nil
        }
        return window
    }

    @MainActor
    static func keyWindow() -> UIImage? {
        guard let window = window() else {
            return nil
        }
        let renderer = UIGraphicsImageRenderer(bounds: window.bounds)
        return renderer.image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: true)
        }
    }

    @MainActor
    static func region(_ rect: CGRect) -> UIImage? {
        guard let window = window(), rect.width > 0, rect.height > 0, window.bounds.intersects(rect) else {
            return nil
        }
        let renderer = UIGraphicsImageRenderer(bounds: rect)
        return renderer.image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: false)
        }
    }
}

struct PromptMarkupTarget: Identifiable {
    let id: String
    let image: UIImage
    let name: String
    let replacing: String?
}

enum PromptMarkupRenderer {
    @MainActor
    static func render(_ image: UIImage, drawing: PKDrawing, canvasSize: CGSize) -> Data? {
        guard canvasSize.width > 0, canvasSize.height > 0 else { return nil }
        let pixels = PromptMarkupRules.pixelSize(
            width: Double(image.size.width * image.scale), height: Double(image.size.height * image.scale))
        let size = CGSize(width: pixels.width, height: pixels.height)
        let bounds = CGRect(origin: .zero, size: size)
        let scale = CGFloat(PromptMarkupRules.inkScale(pixelWidth: pixels.width, canvasWidth: Double(canvasSize.width)))
        var ink: UIImage?
        UITraitCollection(userInterfaceStyle: .light).performAsCurrent {
            ink = drawing.image(from: CGRect(origin: .zero, size: canvasSize), scale: scale)
        }
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let flattened = UIGraphicsImageRenderer(size: size, format: format).image { context in
            UIColor.white.setFill()
            context.fill(bounds)
            image.draw(in: bounds)
            ink?.draw(in: bounds)
        }
        return flattened.jpegData(compressionQuality: 0.95)
    }
}

struct PromptMarkupEditor: View {
    let target: PromptMarkupTarget
    let confirmLabel: String
    let onDone: (PromptAttachment?) -> Void

    @State private var canvas: PKCanvasView = PKCanvasView()
    @State private var saving: Bool = false
    @State private var confirmDiscard: Bool = false
    @State private var problem: String?

    var body: some View {
        NavigationStack {
            GeometryReader { (proxy: GeometryProxy) in
                let fit: PromptMarkupFit = PromptMarkupRules.fit(
                    imageWidth: Double(target.image.size.width), imageHeight: Double(target.image.size.height),
                    boxWidth: Double(proxy.size.width), boxHeight: Double(proxy.size.height))
                ZStack(alignment: .topLeading) {
                    Color.black
                    Image(uiImage: target.image)
                        .resizable()
                        .frame(width: CGFloat(fit.width), height: CGFloat(fit.height))
                        .padding(.leading, CGFloat(fit.x))
                        .padding(.top, CGFloat(fit.y))
                        .accessibilityHidden(true)
                    PromptMarkupCanvas(canvas: canvas)
                        .frame(width: CGFloat(fit.width), height: CGFloat(fit.height))
                        .padding(.leading, CGFloat(fit.x))
                        .padding(.top, CGFloat(fit.y))
                        .accessibilityLabel(Text("Drawing area"))
                }
            }
            .background(Color.black)
            .ignoresSafeArea(.keyboard)
            .navigationTitle("Mark Up")
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(.visible, for: .navigationBar)
            .toolbarColorScheme(.dark, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Discard") { discard() }
                        .disabled(saving)
                        .accessibilityIdentifier("markup-discard")
                }
                ToolbarItem(placement: .confirmationAction) {
                    if saving {
                        ProgressView()
                    } else {
                        Button(confirmLabel) { save() }
                            .fontWeight(.semibold)
                            .accessibilityIdentifier("markup-done")
                    }
                }
            }
            .confirmationDialog("Discard this markup?", isPresented: $confirmDiscard, titleVisibility: .visible) {
                Button("Discard", role: .destructive) { onDone(nil) }
            }
            .alert(
                "Could not save the markup",
                isPresented: Binding<Bool>(get: { problem != nil }, set: { if !$0 { problem = nil } })
            ) {
                Button("Copy") { UIPasteboard.general.string = "Could not save the markup: \(problem ?? "")" }
                Button("Send to agent") { FmMessageHandoff.send("Could not save the markup: \(problem ?? "")") }
                Button("OK", role: .cancel) {}
            } message: {
                Text(problem ?? "")
            }
        }
        .interactiveDismissDisabled()
    }

    private func discard() {
        if canvas.drawing.strokes.isEmpty {
            onDone(nil)
        } else {
            confirmDiscard = true
        }
    }

    private func save() {
        guard !saving else { return }
        saving = true
        guard let data = PromptMarkupRenderer.render(target.image, drawing: canvas.drawing, canvasSize: canvas.bounds.size) else {
            problem = "The picture could not be drawn."
            saving = false
            return
        }
        let name: String = target.name
        Task { @MainActor in
            do {
                onDone(try await PromptPhotoPreparer.prepare(data, name: name))
            } catch let error as PromptAttachmentError {
                problem = error.message
                saving = false
            } catch {
                problem = error.localizedDescription
                saving = false
            }
        }
    }
}

struct PromptMarkupCanvas: UIViewRepresentable {
    let canvas: PKCanvasView

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> PKCanvasView {
        canvas.backgroundColor = .clear
        canvas.isOpaque = false
        canvas.drawingPolicy = .anyInput
        canvas.isScrollEnabled = false
        canvas.overrideUserInterfaceStyle = .light
        canvas.tool = PKInkingTool(.pen, color: .systemRed, width: 6)
        let picker: PKToolPicker = context.coordinator.picker
        picker.colorUserInterfaceStyle = .light
        picker.overrideUserInterfaceStyle = .dark
        picker.addObserver(canvas)
        picker.setVisible(true, forFirstResponder: canvas)
        DispatchQueue.main.async {
            canvas.becomeFirstResponder()
        }
        return canvas
    }

    func updateUIView(_ view: PKCanvasView, context: Context) {}

    static func dismantleUIView(_ view: PKCanvasView, coordinator: Coordinator) {
        coordinator.picker.setVisible(false, forFirstResponder: view)
        coordinator.picker.removeObserver(view)
        view.resignFirstResponder()
    }

    @MainActor
    final class Coordinator {
        let picker: PKToolPicker = PKToolPicker()
    }
}
