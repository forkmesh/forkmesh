import FMCode
import FMDiff
import FMFiles
import PDFKit
import QuickLook
import SwiftUI
import UIKit

struct FileOpenScreen: View {
    let source: FileSource
    let entry: FileEntry

    enum Content {
        case text
        case image(UIImage)
        case pdf(Data)
        case file(URL)
    }

    @State private var content: Content?
    @State private var problem = ""
    @State private var original = ""
    @State private var text = ""
    @State private var caret = CodeCaret.start
    @State private var editing = false
    @State private var indent = "  "
    @State private var saving = false
    @State private var notice = ""
    @State private var noticeIsError = false
    @State private var sharing: SharedFile?

    private var dirty: Bool { text != original }

    private var isText: Bool {
        if case .text? = content { return true }
        return false
    }

    var body: some View {
        Group {
            if !problem.isEmpty {
                ContentUnavailableView(entry.name, systemImage: "doc.questionmark", description: Text(problem))
            } else if let content {
                switch content {
                case .text: editor
                case .image(let image): ZoomableImage(image: image).background(Color.black)
                case .pdf(let data): PDFKitView(data: data)
                case .file(let url): QuickLookView(url: url)
                }
            } else {
                ProgressView("Opening \(entry.name)…")
            }
        }
        .navigationTitle(entry.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                if isText {
                    Button("Save") { Task { await save() } }
                        .disabled(!dirty || saving)
                        .accessibilityIdentifier("files-save")
                }
                Button { Task { await share() } } label: { Image(systemName: "square.and.arrow.up") }
                    .accessibilityLabel("Share")
            }
        }
        .sheet(item: $sharing) { file in ActivitySheet(items: [file.url]) }
        .task { await load() }
    }

    @ViewBuilder private var editor: some View {
        VStack(spacing: 0) {
            if !notice.isEmpty {
                Text(notice)
                    .font(.footnote)
                    .foregroundStyle(noticeIsError ? Color.red : Color.secondary)
                    .padding(.horizontal)
                    .padding(.vertical, 4)
            }
            CodeEditorView(text: $text, language: CodeLang.forPath(entry.name), indentUnit: indent, caret: $caret, focused: $editing)
            LocalEditorStatusBar(
                caret: caret, focused: editing,
                trailing: source.isNetwork ? "On \(source.title)" : CodeLang.forPath(entry.name).label, dirty: dirty)
        }
    }

    private func message(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
    }

    private func load() async {
        guard content == nil else { return }
        do {
            switch entry.kind {
            case .image:
                let data = try await source.read(entry.path, limit: FileKinds.previewLimit, progress: nil)
                if let image = UIImage(data: data) {
                    content = .image(image)
                } else {
                    problem = "This picture cannot be shown."
                }
            case .pdf:
                content = .pdf(try await source.read(entry.path, limit: FileKinds.previewLimit, progress: nil))
            case .text, .other, .folder:
                if entry.size <= FileKinds.textLimit {
                    let data = try await source.read(entry.path, limit: FileKinds.textLimit, progress: nil)
                    if entry.kind == .text || FileKinds.looksLikeText(Array(data.prefix(FileKinds.sniffLength))),
                       let decoded = String(data: data, encoding: .utf8) {
                        original = decoded
                        text = decoded
                        indent = CodeEditing.detectIndentUnit(decoded)
                        content = .text
                        return
                    }
                }
                content = .file(try await localURL())
            }
        } catch {
            problem = message(error)
        }
    }

    private func localURL() async throws -> URL {
        if let local = source as? LocalFileSource { return try local.url(entry.path) }
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("files-open-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let target = try entry.localTarget(in: folder)
        try await source.download(entry.path, to: target, progress: nil)
        return target
    }

    private func save() async {
        saving = true
        defer { saving = false }
        do {
            try await source.write(entry.path, data: Data(text.utf8))
            original = text
            notice = source.isNetwork ? "Saved to \(source.title)." : "Saved."
            noticeIsError = false
        } catch {
            notice = message(error)
            noticeIsError = true
        }
    }

    private func share() async {
        do {
            if case .file(let url)? = content {
                sharing = SharedFile(url: url)
                return
            }
            if isText, dirty {
                let temp = FileManager.default.temporaryDirectory.appendingPathComponent("files-share-\(UUID().uuidString)", isDirectory: true)
                try FileManager.default.createDirectory(at: temp, withIntermediateDirectories: true)
                let target = try entry.localTarget(in: temp)
                try Data(text.utf8).write(to: target)
                sharing = SharedFile(url: target)
                return
            }
            sharing = SharedFile(url: try await localURL())
        } catch {
            notice = message(error)
            noticeIsError = true
        }
    }
}

struct ZoomableImage: UIViewRepresentable {
    let image: UIImage

    func makeUIView(context: Context) -> UIScrollView {
        let scroll = UIScrollView()
        scroll.minimumZoomScale = 1
        scroll.maximumZoomScale = 8
        scroll.delegate = context.coordinator
        scroll.showsHorizontalScrollIndicator = false
        scroll.showsVerticalScrollIndicator = false
        let view = UIImageView(image: image)
        view.contentMode = .scaleAspectFit
        view.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(view)
        NSLayoutConstraint.activate([
            view.widthAnchor.constraint(equalTo: scroll.frameLayoutGuide.widthAnchor),
            view.heightAnchor.constraint(equalTo: scroll.frameLayoutGuide.heightAnchor),
            view.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor),
            view.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor),
            view.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor),
            view.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor),
        ])
        context.coordinator.imageView = view
        let tap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.toggle(_:)))
        tap.numberOfTapsRequired = 2
        scroll.addGestureRecognizer(tap)
        return scroll
    }

    func updateUIView(_ view: UIScrollView, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject, UIScrollViewDelegate {
        weak var imageView: UIImageView?

        func viewForZooming(in scrollView: UIScrollView) -> UIView? { imageView }

        @objc func toggle(_ gesture: UITapGestureRecognizer) {
            guard let scroll = gesture.view as? UIScrollView else { return }
            scroll.setZoomScale(scroll.zoomScale > 1 ? 1 : 3, animated: true)
        }
    }
}

struct PDFKitView: UIViewRepresentable {
    let data: Data

    func makeUIView(context: Context) -> PDFView {
        let view = PDFView()
        view.autoScales = true
        view.document = PDFDocument(data: data)
        return view
    }

    func updateUIView(_ view: PDFView, context: Context) {}
}

struct QuickLookView: UIViewControllerRepresentable {
    let url: URL

    func makeUIViewController(context: Context) -> QLPreviewController {
        let controller = QLPreviewController()
        controller.dataSource = context.coordinator
        return controller
    }

    func updateUIViewController(_ controller: QLPreviewController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(url: url) }

    final class Coordinator: NSObject, QLPreviewControllerDataSource {
        let url: URL
        init(url: URL) { self.url = url }
        func numberOfPreviewItems(in controller: QLPreviewController) -> Int { 1 }
        func previewController(_ controller: QLPreviewController, previewItemAt index: Int) -> QLPreviewItem { url as NSURL }
    }
}
