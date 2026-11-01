import FMTools
import SwiftUI
import UIKit
import WebKit

private let noteMutedInk = Color(red: 0x57 / 255.0, green: 0x60 / 255.0, blue: 0x6A / 255.0)

struct NoteFigureView: View {
    @Environment(\.fmPalette) private var fm
    let figure: NoteFigure

    var body: some View {
        let size: NoteFigureSize = figure.size
        VStack(spacing: FmSpace.x2) {
            NoteSvgImage(page: figure.page)
                .aspectRatio(CGFloat(1 / size.aspect), contentMode: .fit)
                .frame(maxWidth: size.width.map { (width: Double) -> CGFloat in CGFloat(width) } ?? .infinity)
                .frame(maxWidth: .infinity)
            if !figure.caption.isEmpty {
                Text(figure.caption)
                    .fmText(.bodySmall, color: noteMutedInk)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity)
            }
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity)
        .background(Color.white)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous).strokeBorder(fm.border, lineWidth: 1)
        }
        .padding(.vertical, FmSpace.x1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(figure.label))
        .accessibilityAddTraits(.isImage)
    }
}

private struct NoteSvgImage: UIViewRepresentable {
    let page: String

    func makeUIView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.isOpaque = false
        view.backgroundColor = .clear
        view.scrollView.backgroundColor = .clear
        view.scrollView.isScrollEnabled = false
        view.scrollView.bounces = false
        view.isUserInteractionEnabled = false
        return view
    }

    func updateUIView(_ view: WKWebView, context: Context) {
        guard context.coordinator.shown != page else { return }
        context.coordinator.shown = page
        view.loadHTMLString(page, baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator {
        var shown: String?
    }
}

struct NotePageBreakView: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            rule
            Text(NoteDocument.pageBreakLabel.uppercased())
                .fmText(.labelSmall, color: fm.textTertiary, tracking: 0.6)
                .lineLimit(1)
                .fixedSize()
            rule
        }
        .padding(.vertical, FmSpace.x3)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(NoteDocument.pageBreakLabel))
    }

    private var rule: some View {
        NoteDashedRule()
            .stroke(fm.border, style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
            .frame(height: 1)
            .frame(maxWidth: .infinity)
    }
}

private struct NoteDashedRule: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.midY))
        return path
    }
}

struct NotePdfSheet: View {
    @Environment(\.fmPalette) private var fm
    let model: NoteEditorModel
    let onClose: () -> Void
    @State private var layout: NotePrintLayout
    @State private var saves: Bool
    @State private var busy: Bool = false
    @State private var notice: FmNotice?

    init(model: NoteEditorModel, onClose: @escaping () -> Void) {
        self.model = model
        self.onClose = onClose
        _layout = State(initialValue: model.pageLayout)
        _saves = State(initialValue: model.canEdit)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                FmPanelHeader(title: NoteDocument.exportTitle)
                choice(
                    NoteDocument.paperLabel, selection: $layout.size, options: NotePrintLayout.Size.allCases,
                    label: { (size: NotePrintLayout.Size) -> String in size.label }, id: "note-pdf-size")
                choice(
                    NoteDocument.orientationLabel, selection: $layout.orientation, options: NotePrintLayout.Orientation.allCases,
                    label: { (orientation: NotePrintLayout.Orientation) -> String in orientation.label }, id: "note-pdf-orientation")
                choice(
                    NoteDocument.marginsLabel, selection: $layout.margins, options: NotePrintLayout.Margins.allCases,
                    label: { (margins: NotePrintLayout.Margins) -> String in margins.label }, id: "note-pdf-margins")
                toggle(NoteDocument.coverLabel, isOn: $layout.cover, id: "note-pdf-cover")
                toggle(NoteDocument.contentsLabel, isOn: $layout.contents, id: "note-pdf-contents")
                toggle(NoteDocument.numbersLabel, isOn: $layout.numbers, id: "note-pdf-numbers")
                if model.canEdit {
                    FmHairline()
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.vertical, FmSpace.x1)
                    toggle(NoteDocument.saveLayoutLabel, isOn: $saves, id: "note-pdf-save")
                }
                if notice != nil {
                    FmNoticeLine($notice)
                        .padding(.horizontal, FmSpace.x4)
                }
                FmFilledButton(NoteDocument.exportButton, fillWidth: true, busy: busy, onPressed: busy ? nil : export)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x3)
                    .padding(.bottom, FmSpace.x4)
                    .accessibilityIdentifier("note-pdf-export")
            }
        }
        .fmSheetStyle(fitsContent: false)
    }

    private func choice<Option: Hashable>(
        _ title: String, selection: Binding<Option>, options: [Option], label: @escaping (Option) -> String, id: String
    ) -> some View {
        HStack(spacing: FmSpace.x2) {
            Text(title)
                .fmText(.bodyMedium)
            Spacer(minLength: FmSpace.x2)
            Picker(title, selection: selection) {
                ForEach(options, id: \.self) { (option: Option) in
                    Text(label(option)).tag(option)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .tint(fm.accent)
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(minHeight: 48)
        .accessibilityIdentifier(id)
    }

    private func toggle(_ title: String, isOn: Binding<Bool>, id: String) -> some View {
        Toggle(title, isOn: isOn)
            .fmText(.bodyMedium)
            .tint(fm.accent)
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 48)
            .accessibilityIdentifier(id)
    }

    private func export() {
        busy = true
        notice = nil
        let chosen: NotePrintLayout = layout
        if saves && model.canEdit {
            model.savePageLayout(chosen)
        }
        let html: String = model.printHTML(chosen, date: Date())
        let name: String = model.pdfFileName
        Task { @MainActor in
            do {
                let data: Data = try await NotePdfRenderer.render(html: html, layout: chosen)
                let url: URL = try NoteDocument.write(data, fileName: name, in: FileManager.default.temporaryDirectory)
                busy = false
                NotePdfShare.present(url, onDone: onClose)
            } catch {
                busy = false
                notice = FmNotice.error(NoteDocument.failedMessage)
            }
        }
    }
}

enum NotePdfError: Error {
    case timedOut
    case empty
}

@MainActor
final class NotePdfRenderer: NSObject, WKNavigationDelegate {
    private var waiting: CheckedContinuation<Void, Error>?

    static func render(html: String, layout: NotePrintLayout) async throws -> Data {
        let renderer = NotePdfRenderer()
        return try await renderer.run(html: html, layout: layout)
    }

    private func run(html: String, layout: NotePrintLayout) async throws -> Data {
        let paper = CGRect(x: 0, y: 0, width: CGFloat(layout.paperPoints.width), height: CGFloat(layout.paperPoints.height))
        let margin = CGFloat(layout.marginPoints)
        let printable: CGRect = paper.insetBy(dx: margin, dy: margin)
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
        let frame = CGRect(x: -printable.width * 2, y: 0, width: printable.width, height: printable.height)
        let view = WKWebView(frame: frame, configuration: configuration)
        view.isUserInteractionEnabled = false
        view.navigationDelegate = self
        NotePdfShare.keyWindow()?.insertSubview(view, at: 0)
        defer {
            view.navigationDelegate = nil
            view.removeFromSuperview()
        }
        let timeout = Task { @MainActor [weak self] in
            do {
                try await Task.sleep(nanoseconds: 30_000_000_000)
            } catch {
                return
            }
            self?.finish(NotePdfError.timedOut)
        }
        defer { timeout.cancel() }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            waiting = continuation
            view.loadHTMLString(html, baseURL: nil)
        }
        try? await Task.sleep(nanoseconds: 200_000_000)
        let pages = NotePdfPages(paper: paper, printable: printable, layout: layout)
        pages.addPrintFormatter(view.viewPrintFormatter(), startingAtPageAt: 0)
        let data = NSMutableData()
        UIGraphicsBeginPDFContextToData(data, paper, nil)
        let count: Int = pages.numberOfPages
        pages.total = count
        pages.prepare(forDrawingPages: NSRange(location: 0, length: count))
        let bounds: CGRect = UIGraphicsGetPDFContextBounds()
        for index in 0..<count {
            UIGraphicsBeginPDFPage()
            pages.drawPage(at: index, in: bounds)
        }
        UIGraphicsEndPDFContext()
        guard count > 0 else { throw NotePdfError.empty }
        return data as Data
    }

    private func finish(_ error: Error?) {
        guard let continuation = waiting else { return }
        waiting = nil
        if let error = error {
            continuation.resume(throwing: error)
        } else {
            continuation.resume()
        }
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        finish(nil)
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        finish(error)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        finish(error)
    }
}

final class NotePdfPages: UIPrintPageRenderer {
    private let paper: CGRect
    private let printable: CGRect
    private let layout: NotePrintLayout
    var total: Int = 0

    init(paper: CGRect, printable: CGRect, layout: NotePrintLayout) {
        self.paper = paper
        self.printable = printable
        self.layout = layout
        super.init()
    }

    override var paperRect: CGRect { paper }

    override var printableRect: CGRect { printable }

    override func drawPage(at pageIndex: Int, in printableRect: CGRect) {
        super.drawPage(at: pageIndex, in: printableRect)
        guard NoteDocument.showsNumber(onPage: pageIndex, layout: layout) else { return }
        let text: NSString = NoteDocument.pageNumber(pageIndex + 1, of: total) as NSString
        let attributes: [NSAttributedString.Key: Any] = [
            .font: UIFont.systemFont(ofSize: 9),
            .foregroundColor: UIColor(red: 0x57 / 255.0, green: 0x60 / 255.0, blue: 0x6A / 255.0, alpha: 1),
        ]
        let size: CGSize = text.size(withAttributes: attributes)
        let band: CGFloat = paper.maxY - printable.maxY
        let point = CGPoint(x: paper.midX - size.width / 2, y: printable.maxY + (band - size.height) / 2)
        text.draw(at: point, withAttributes: attributes)
    }
}

@MainActor
enum NotePdfShare {
    static func keyWindow() -> UIWindow? {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        return scene?.windows.first(where: { $0.isKeyWindow }) ?? scene?.windows.first
    }

    static func present(_ url: URL, onDone: @escaping () -> Void) {
        guard var top: UIViewController = keyWindow()?.rootViewController else {
            onDone()
            return
        }
        while let next: UIViewController = top.presentedViewController, !next.isBeingDismissed {
            top = next
        }
        let controller = UIActivityViewController(activityItems: [url], applicationActivities: nil)
        controller.completionWithItemsHandler = { (_: UIActivity.ActivityType?, _: Bool, _: [Any]?, _: Error?) in
            onDone()
        }
        if let popover: UIPopoverPresentationController = controller.popoverPresentationController {
            popover.sourceView = top.view
            popover.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 1, height: 1)
            popover.permittedArrowDirections = []
        }
        top.present(controller, animated: true)
    }
}
