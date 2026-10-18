import FMAccount
import FMAgents
import FMApi
import FMBase
import Observation
import SwiftUI
import UIKit

@MainActor
enum CaptureHold {
    static let holdSeconds: TimeInterval = 0.6
    static let allowedMovement: CGFloat = 10
    static let handledGrace: TimeInterval = 1
    static let captureDelay: Duration = .milliseconds(350)
    static let noticeTime: Duration = .milliseconds(1400)

    private static let guide: CaptureHoldGuide = CaptureHoldGuide()
    private static let menu: CaptureHoldMenu = CaptureHoldMenu()
    private static var handledAt: Date = .distantPast
    private static var overlay: CaptureOverlay?
    private static var starting: Bool = false

    static var isOn: Bool { Services.get(SettingsService.self)?.captureOn ?? true }

    static var isIdle: Bool { overlay == nil && !starting }

    static func noteHandled() {
        handledAt = Date()
    }

    static func install(on window: UIWindow) {
        if window.gestureRecognizers?.contains(where: { $0 is CaptureHoldRecognizer }) == true { return }
        let hold: CaptureHoldRecognizer = CaptureHoldRecognizer(target: guide, action: #selector(CaptureHoldGuide.held(_:)))
        hold.minimumPressDuration = holdSeconds
        hold.allowableMovement = allowedMovement
        hold.cancelsTouchesInView = false
        hold.delaysTouchesBegan = false
        hold.delaysTouchesEnded = false
        hold.delegate = guide
        window.addGestureRecognizer(hold)
    }

    static func shouldBegin(at point: CGPoint, in window: UIWindow) -> Bool {
        guard isOn, isIdle, Date().timeIntervalSince(handledAt) > handledGrace else { return false }
        return isBlank(at: point, in: window)
    }

    static func held(at point: CGPoint, in window: UIWindow) {
        guard Date().timeIntervalSince(handledAt) > handledGrace else { return }
        noteHandled()
        FmHaptics.selection()
        menu.present(at: point, in: window)
    }

    static func isBlank(at point: CGPoint, in window: UIWindow) -> Bool {
        guard let hit = window.hitTest(point, with: nil) else { return false }
        if EyeClicker.ancestor(of: hit, where: isContentView) != nil { return false }
        let screen: UIView = EyeClicker.ancestor(of: hit, where: { $0.next is UIViewController }) ?? window
        let screenPoint: CGPoint = window.convert(point, to: window.screen.coordinateSpace)
        return !hasElement(at: screenPoint, under: screen, bounds: window.bounds.size)
    }

    private static func isContentView(_ view: UIView) -> Bool {
        switch view {
        case let control as UIControl: return control.isEnabled
        case is UITextView, is UITextField, is UISearchBar, is UITableViewCell, is UICollectionViewCell: return true
        default: return false
        }
    }

    private static func hasElement(at point: CGPoint, under root: UIView, bounds: CGSize) -> Bool {
        var visited: Int = 0
        func visit(_ node: NSObject, depth: Int) -> Bool {
            guard depth < EyeClicker.maxDepth, visited < EyeClicker.maxNodes else { return false }
            visited += 1
            if node.accessibilityElementsHidden { return false }
            if let view = node as? UIView, view.isHidden || view.alpha < 0.01 { return false }
            if node.isAccessibilityElement {
                let frame: CGRect = node.accessibilityFrame
                return frame.contains(point)
                    && CaptureAreaRules.isContent(
                        elementWidth: Double(frame.width), elementHeight: Double(frame.height),
                        boundsWidth: Double(bounds.width), boundsHeight: Double(bounds.height))
            }
            for child in EyeClicker.children(of: node) where visit(child, depth: depth + 1) {
                return true
            }
            return false
        }
        return visit(root, depth: 0)
    }

    static func topView(in window: UIWindow) -> UIView {
        var top: UIViewController? = window.rootViewController
        while let next = top?.presentedViewController, !next.isBeingDismissed {
            top = next
        }
        return top?.view ?? window
    }

    static func start(whole: Bool) {
        guard isIdle else { return }
        starting = true
        Task { @MainActor in
            try? await Task.sleep(for: CaptureHold.captureDelay)
            CaptureHold.starting = false
            CaptureHold.open(whole: whole)
        }
    }

    private static func open(whole: Bool) {
        guard overlay == nil, let window = PromptScreenCapture.window(), let scene = window.windowScene else { return }
        let session: CaptureSession
        if let shot = PromptScreenCapture.keyWindow() {
            session = CaptureSession(stage: whole ? CaptureSession.markingStage(shot) : .selecting(shot))
        } else {
            FmHaptics.warning()
            session = CaptureSession(stage: .notice("This screen could not be captured."))
        }
        let shown: CaptureOverlay = CaptureOverlay(scene: scene, session: session, returnTo: window)
        overlay = shown
        session.onClose = { [weak shown] in
            shown?.close()
            if CaptureHold.overlay === shown {
                CaptureHold.overlay = nil
            }
        }
        shown.show()
        if session.passesTouches {
            session.closeSoon()
        }
    }
}

final class CaptureHoldRecognizer: UILongPressGestureRecognizer {}

@MainActor
final class CaptureHoldGuide: NSObject, UIGestureRecognizerDelegate {
    @objc func held(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began, let window = recognizer.view as? UIWindow else { return }
        CaptureHold.held(at: recognizer.location(in: window), in: window)
    }

    func gestureRecognizer(_ recognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        CaptureHold.isOn && CaptureHold.isIdle
    }

    func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
        guard let window = recognizer.view as? UIWindow else { return false }
        return CaptureHold.shouldBegin(at: recognizer.location(in: window), in: window)
    }

    func gestureRecognizer(
        _ recognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer
    ) -> Bool {
        true
    }
}

@MainActor
final class CaptureHoldMenu: NSObject, UIEditMenuInteractionDelegate {
    private var interaction: UIEditMenuInteraction?

    func present(at point: CGPoint, in window: UIWindow) {
        if let interaction {
            interaction.view?.removeInteraction(interaction)
        }
        let host: UIView = CaptureHold.topView(in: window)
        let next: UIEditMenuInteraction = UIEditMenuInteraction(delegate: self)
        host.addInteraction(next)
        interaction = next
        next.presentEditMenu(with: UIEditMenuConfiguration(identifier: nil, sourcePoint: host.convert(point, from: window)))
    }

    func editMenuInteraction(
        _ interaction: UIEditMenuInteraction, menuFor configuration: UIEditMenuConfiguration,
        suggestedActions: [UIMenuElement]
    ) -> UIMenu? {
        UIMenu(children: [
            UIAction(title: "Select Area", image: UIImage(systemName: "crop")) { (_: UIAction) in
                CaptureHold.start(whole: false)
            },
            UIAction(title: "Whole Screen", image: UIImage(systemName: "rectangle.dashed")) { (_: UIAction) in
                CaptureHold.start(whole: true)
            },
        ])
    }
}

struct CaptureHoldAnchor: UIViewRepresentable {
    func makeUIView(context: Context) -> CaptureHoldAnchorView { CaptureHoldAnchorView() }

    func updateUIView(_ view: CaptureHoldAnchorView, context: Context) {}
}

final class CaptureHoldAnchorView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        accessibilityElementsHidden = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if let window {
            CaptureHold.install(on: window)
        }
    }
}

@MainActor
@Observable
final class CaptureSession {
    enum Stage {
        case selecting(UIImage)
        case marking(PromptMarkupTarget)
        case notice(String)
    }

    var stage: Stage
    @ObservationIgnored var onClose: () -> Void = {}

    init(stage: Stage) {
        self.stage = stage
    }

    var passesTouches: Bool {
        if case .notice = stage { return true }
        return false
    }

    static func markingStage(_ image: UIImage) -> Stage {
        .marking(PromptMarkupTarget(id: PromptPhotoRules.newId(), image: image, name: PromptMarkupRules.captureName, replacing: nil))
    }

    func pick(_ area: CaptureArea) {
        guard case .selecting(let shot) = stage else { return }
        guard let image = CaptureSession.crop(shot, to: area) else {
            notify("That area could not be copied.")
            return
        }
        stage = CaptureSession.markingStage(image)
    }

    func markWhole() {
        guard case .selecting(let shot) = stage else { return }
        stage = CaptureSession.markingStage(shot)
    }

    func copy(_ image: UIImage) {
        UIPasteboard.general.image = image
        FmHaptics.success()
        notify("Copied to clipboard")
    }

    func cancel() {
        onClose()
    }

    func notify(_ text: String) {
        stage = .notice(text)
        closeSoon()
    }

    func closeSoon() {
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: CaptureHold.noticeTime)
            self?.onClose()
        }
    }

    static func crop(_ image: UIImage, to area: CaptureArea) -> UIImage? {
        guard let source = image.cgImage else { return nil }
        let pixels: CaptureArea = CaptureAreaRules.pixels(
            area, scale: Double(image.scale), pixelWidth: source.width, pixelHeight: source.height)
        guard pixels.width >= 1, pixels.height >= 1,
            let cropped = source.cropping(to: CGRect(x: pixels.x, y: pixels.y, width: pixels.width, height: pixels.height))
        else { return nil }
        return UIImage(cgImage: cropped, scale: image.scale, orientation: .up)
    }
}

final class CaptureOverlayWindow: UIWindow {
    weak var session: CaptureSession?

    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        if session?.passesTouches ?? true { return nil }
        return super.hitTest(point, with: event)
    }
}

@MainActor
final class CaptureOverlay {
    private let window: CaptureOverlayWindow
    private weak var returnTo: UIWindow?
    private let session: CaptureSession

    init(scene: UIWindowScene, session: CaptureSession, returnTo: UIWindow) {
        self.session = session
        self.returnTo = returnTo
        window = CaptureOverlayWindow(windowScene: scene)
        window.windowLevel = UIWindow.Level.alert + 1
        window.backgroundColor = .clear
        window.session = session
    }

    func show() {
        let host: UIHostingController<CaptureOverlayView> = UIHostingController(rootView: CaptureOverlayView(session: session))
        host.view.backgroundColor = .clear
        window.rootViewController = host
        window.makeKeyAndVisible()
    }

    func close() {
        window.isHidden = true
        window.rootViewController = nil
        returnTo?.makeKey()
    }
}

struct CaptureOverlayView: View {
    let session: CaptureSession

    var body: some View {
        switch session.stage {
        case .selecting(let shot):
            CaptureAreaPicker(
                shot: shot, onPick: { (area: CaptureArea) in session.pick(area) }, onWhole: { session.markWhole() },
                onCancel: { session.cancel() })
        case .marking(let target):
            PromptMarkupEditor(
                target: target, confirmLabel: "Copy", onDone: { (_: PromptAttachment?) in session.cancel() },
                onCopy: { (image: UIImage) in session.copy(image) })
        case .notice(let text):
            CaptureNotice(text: text)
        }
    }
}

struct CaptureAreaPicker: View {
    let shot: UIImage
    let onPick: (CaptureArea) -> Void
    let onWhole: () -> Void
    let onCancel: () -> Void

    @State private var from: CGPoint?
    @State private var to: CGPoint?

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let box: CGRect = selection(in: proxy.size)
            ZStack(alignment: .topLeading) {
                Image(uiImage: shot)
                    .resizable()
                    .frame(width: proxy.size.width, height: proxy.size.height)
                    .accessibilityHidden(true)
                CaptureShade(hole: box)
                    .fill(Color.black.opacity(0.45), style: FillStyle(eoFill: true))
                    .accessibilityHidden(true)
                if box.width > 0, box.height > 0 {
                    Rectangle()
                        .strokeBorder(Color.white, style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
                        .frame(width: box.width, height: box.height)
                        .offset(x: box.minX, y: box.minY)
                        .accessibilityHidden(true)
                }
            }
            .frame(width: proxy.size.width, height: proxy.size.height)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { (value: DragGesture.Value) in
                        from = value.startLocation
                        to = value.location
                    }
                    .onEnded { (value: DragGesture.Value) in
                        let area: CaptureArea = CaptureAreaRules.area(
                            fromX: Double(value.startLocation.x), fromY: Double(value.startLocation.y),
                            toX: Double(value.location.x), toY: Double(value.location.y),
                            boundsWidth: Double(proxy.size.width), boundsHeight: Double(proxy.size.height))
                        if CaptureAreaRules.isUsable(area) {
                            FmHaptics.selection()
                            onPick(area)
                        } else {
                            from = nil
                            to = nil
                        }
                    })
        }
        .ignoresSafeArea()
        .overlay(alignment: .top) { bar }
        .accessibilityIdentifier("capture-area-picker")
    }

    private func selection(in size: CGSize) -> CGRect {
        guard let from, let to else { return .zero }
        let area: CaptureArea = CaptureAreaRules.area(
            fromX: Double(from.x), fromY: Double(from.y), toX: Double(to.x), toY: Double(to.y),
            boundsWidth: Double(size.width), boundsHeight: Double(size.height))
        return CGRect(x: area.x, y: area.y, width: area.width, height: area.height)
    }

    private var bar: some View {
        HStack(spacing: FmSpace.x3) {
            Text("Drag over the area to copy")
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
                .minimumScaleFactor(0.8)
            Spacer(minLength: FmSpace.x1)
            Button("Whole Screen") { onWhole() }
                .font(.subheadline)
                .accessibilityIdentifier("capture-area-whole")
            Button("Cancel") { onCancel() }
                .font(.subheadline.weight(.semibold))
                .accessibilityIdentifier("capture-area-cancel")
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
        .background(.regularMaterial, in: Capsule())
        .padding(.horizontal, FmSpace.x3)
        .padding(.top, FmSpace.x2)
    }
}

struct CaptureShade: Shape {
    let hole: CGRect

    func path(in rect: CGRect) -> Path {
        var path: Path = Path(rect)
        if hole.width > 0, hole.height > 0 {
            path.addRect(hole)
        }
        return path
    }
}

struct CaptureNotice: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.subheadline.weight(.semibold))
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x3)
            .background(.regularMaterial, in: Capsule())
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .padding(.top, FmSpace.x2)
            .allowsHitTesting(false)
            .onAppear { UIAccessibility.post(notification: .announcement, argument: text) }
    }
}
