import FMPackages
import SwiftUI
import UIKit

struct SecureDriveEditorScreen: View {
    let model: SecureDriveModel
    @State private var current: SecureDriveEditorTarget
    @State private var latest: [UInt8]?
    @State private var dirty = false
    @State private var asking = false
    @State private var note = ""
    @Environment(\.dismiss) private var dismiss

    init(model: SecureDriveModel, target: SecureDriveEditorTarget) {
        self.model = model
        _current = State(initialValue: target)
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                content
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                Text(note.isEmpty ? footer : note)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                    .background(.bar)
            }
            .navigationTitle((dirty ? "\u{2022} " : "") + current.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(current.readOnly ? "Done" : "Close") {
                        if dirty { asking = true } else { close() }
                    }
                }
                if !current.readOnly {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Save") { Task { await save(closing: false) } }
                            .disabled(!dirty || model.busy)
                            .accessibilityIdentifier("secure-drive-editor-save")
                    }
                }
            }
            .confirmationDialog("Save changes?", isPresented: $asking, titleVisibility: .visible) {
                Button("Save") { Task { await save(closing: true) } }
                Button("Discard Changes", role: .destructive) { close() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("\(current.name) has changes that are not on the drive yet.")
            }
        }
        .interactiveDismissDisabled(dirty)
    }

    private var footer: String {
        current.readOnly
            ? "Read only. Decrypted in memory; nothing is written to this iPhone's storage."
            : "Decrypted in memory; nothing is written to this iPhone's storage. Each save is a new encrypted version on the drive."
    }

    @ViewBuilder private var content: some View {
        switch current.kind {
        case "document":
            SecureDriveDocumentEditor(bytes: current.bytes, readOnly: current.readOnly, onEdit: edited)
        case "text":
            SecureDriveTextEditor(bytes: current.bytes, readOnly: current.readOnly, onEdit: edited)
        case "spreadsheet":
            SecureDriveSheetEditor(sheet: SecureDriveSheet.fromCsv(current.bytes), readOnly: current.readOnly, onEdit: edited)
        case "drawing":
            if let drawing = SecureDriveDrawing.fromSvg(current.bytes) {
                SecureDriveDrawingEditor(drawing: drawing, readOnly: current.readOnly, onEdit: edited)
            } else {
                SecureDriveNoViewer(model: model, fileId: current.fileId, text: "This picture cannot be shown here. Use Open in another app or Save a decrypted copy.")
            }
        default:
            SecureDriveImageViewer(model: model, fileId: current.fileId, bytes: current.bytes)
        }
    }

    private func edited(_ bytes: [UInt8]) {
        latest = bytes
        if !dirty {
            dirty = true
            model.editorDirty = true
        }
        model.touch()
    }

    private func save(closing: Bool) async {
        guard let bytes = latest else {
            if closing { close() }
            return
        }
        if let next = await model.save(current, bytes: bytes) {
            current = next
            latest = nil
            dirty = false
            note = "Saved a new encrypted version to the drive."
            if closing { close() }
        } else {
            note = model.status
            model.editorDirty = dirty
        }
    }

    private func close() {
        dirty = false
        model.editorDirty = false
        model.editor = nil
        dismiss()
    }
}

struct SecureDriveNoViewer: View {
    let model: SecureDriveModel
    let fileId: String
    let text: String

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "doc.questionmark").font(.largeTitle).foregroundStyle(.secondary)
            Text(text).multilineTextAlignment(.center).foregroundStyle(.secondary)
            Button("Open in another app") {
                model.editor = nil
                Task { await model.openExternally(fileId) }
            }
            .buttonStyle(.bordered)
        }
        .padding(32)
    }
}

struct SecureDriveImageViewer: View {
    let model: SecureDriveModel
    let fileId: String
    let bytes: [UInt8]
    @State private var image: UIImage?
    @State private var scale: CGFloat = 1

    var body: some View {
        Group {
            if let image {
                GeometryReader { (proxy: GeometryProxy) in
                    ScrollView([.horizontal, .vertical]) {
                        Image(uiImage: image)
                            .resizable()
                            .scaledToFit()
                            .frame(width: proxy.size.width * scale)
                            .accessibilityLabel("\(Int(image.size.width)) \u{00D7} \(Int(image.size.height)) picture")
                    }
                    .gesture(MagnificationGesture().onChanged { (value: CGFloat) in scale = min(max(value, 1), 6) })
                }
            } else {
                SecureDriveNoViewer(model: model, fileId: fileId, text: "This picture cannot be shown here. Use Open in another app or Save a decrypted copy.")
            }
        }
        .onAppear { image = UIImage(data: Data(bytes)) }
    }
}

struct SecureDriveTextEditor: View {
    let readOnly: Bool
    let onEdit: ([UInt8]) -> Void
    @State private var text: String

    init(bytes: [UInt8], readOnly: Bool, onEdit: @escaping ([UInt8]) -> Void) {
        self.readOnly = readOnly
        self.onEdit = onEdit
        _text = State(initialValue: String(decoding: bytes, as: UTF8.self))
    }

    var body: some View {
        TextEditor(text: $text)
            .font(.body.monospaced())
            .disabled(readOnly)
            .autocorrectionDisabled()
            .textInputAutocapitalization(.never)
            .padding(.horizontal, 8)
            .onChange(of: text) { (_: String, value: String) in
                if !readOnly { onEdit(Array(value.utf8)) }
            }
    }
}

@MainActor
final class SecureDriveRichTextController {
    weak var view: UITextView?
    var onEdit: (([UInt8]) -> Void)?

    static let bodySize: CGFloat = 17
    static let h2Size: CGFloat = 22
    static let h1Size: CGFloat = 28

    func changed() {
        guard let view else { return }
        onEdit?(Array(SecureDriveHTML.write(view.attributedText).utf8))
    }

    func toggle(_ trait: UIFontDescriptor.SymbolicTraits) {
        guard let view else { return }
        let range = view.selectedRange
        if range.length == 0 {
            var attributes = view.typingAttributes
            let font = (attributes[.font] as? UIFont) ?? UIFont.systemFont(ofSize: Self.bodySize)
            attributes[.font] = Self.font(font, toggling: trait)
            view.typingAttributes = attributes
            return
        }
        let text = NSMutableAttributedString(attributedString: view.attributedText)
        var allHave = true
        text.enumerateAttribute(.font, in: range) { value, _, _ in
            let font = (value as? UIFont) ?? UIFont.systemFont(ofSize: Self.bodySize)
            if !font.fontDescriptor.symbolicTraits.contains(trait) { allHave = false }
        }
        text.enumerateAttribute(.font, in: range) { value, sub, _ in
            let font = (value as? UIFont) ?? UIFont.systemFont(ofSize: Self.bodySize)
            var traits = font.fontDescriptor.symbolicTraits
            if allHave { traits.remove(trait) } else { traits.insert(trait) }
            text.addAttribute(.font, value: Self.font(font, traits: traits), range: sub)
        }
        apply(text, range)
    }

    func toggleUnderline() {
        guard let view else { return }
        let range = view.selectedRange
        if range.length == 0 {
            var attributes = view.typingAttributes
            let on = (attributes[.underlineStyle] as? Int ?? 0) != 0
            attributes[.underlineStyle] = on ? 0 : NSUnderlineStyle.single.rawValue
            view.typingAttributes = attributes
            return
        }
        let text = NSMutableAttributedString(attributedString: view.attributedText)
        var allHave = true
        text.enumerateAttribute(.underlineStyle, in: range) { value, _, _ in
            if (value as? Int ?? 0) == 0 { allHave = false }
        }
        if allHave {
            text.removeAttribute(.underlineStyle, range: range)
        } else {
            text.addAttribute(.underlineStyle, value: NSUnderlineStyle.single.rawValue, range: range)
        }
        apply(text, range)
    }

    func setLevel(_ level: Int) {
        guard let view else { return }
        let text = NSMutableAttributedString(attributedString: view.attributedText)
        let string = text.string as NSString
        let paragraphs = string.paragraphRange(for: view.selectedRange)
        let size = level == 1 ? Self.h1Size : level == 2 ? Self.h2Size : Self.bodySize
        if paragraphs.length == 0 {
            var attributes = view.typingAttributes
            attributes[.font] = UIFont.systemFont(ofSize: size, weight: level == 0 ? .regular : .bold)
            view.typingAttributes = attributes
            return
        }
        text.enumerateAttribute(.font, in: paragraphs) { value, sub, _ in
            let font = (value as? UIFont) ?? UIFont.systemFont(ofSize: Self.bodySize)
            var traits = font.fontDescriptor.symbolicTraits
            if level == 0 { traits.remove(.traitBold) } else { traits.insert(.traitBold) }
            text.addAttribute(.font, value: Self.font(UIFont.systemFont(ofSize: size), traits: traits), range: sub)
        }
        apply(text, view.selectedRange)
    }

    func toggleList() {
        guard let view else { return }
        let text = NSMutableAttributedString(attributedString: view.attributedText)
        var selection = view.selectedRange
        let paragraphs = (text.string as NSString).paragraphRange(for: selection)
        var starts = [Int]()
        (text.string as NSString).enumerateSubstrings(in: paragraphs, options: [.byParagraphs, .substringNotRequired]) { _, range, _, _ in
            starts.append(range.location)
        }
        if starts.isEmpty { starts = [paragraphs.location] }
        let allListed = starts.allSatisfy { SecureDriveHTML.bulletLength(text.string as NSString, at: $0) > 0 }
        for start in starts.reversed() {
            let length = SecureDriveHTML.bulletLength(text.string as NSString, at: start)
            if allListed {
                text.deleteCharacters(in: NSRange(location: start, length: length))
                selection.length = max(0, selection.length - length)
                if selection.location > start { selection.location = max(start, selection.location - length) }
            } else if length == 0 {
                let attributes = text.length > 0 ? text.attributes(at: min(start, text.length - 1), effectiveRange: nil) : view.typingAttributes
                text.insert(NSAttributedString(string: "\u{2022} ", attributes: attributes), at: start)
                if selection.location >= start { selection.location += 2 }
            }
        }
        apply(text, NSRange(location: min(selection.location, text.length), length: min(selection.length, max(0, text.length - selection.location))))
    }

    private func apply(_ text: NSAttributedString, _ selection: NSRange) {
        guard let view else { return }
        view.attributedText = text
        view.selectedRange = selection
        changed()
    }

    static func font(_ font: UIFont, toggling trait: UIFontDescriptor.SymbolicTraits) -> UIFont {
        var traits = font.fontDescriptor.symbolicTraits
        if traits.contains(trait) { traits.remove(trait) } else { traits.insert(trait) }
        return Self.font(font, traits: traits)
    }

    static func font(_ font: UIFont, traits: UIFontDescriptor.SymbolicTraits) -> UIFont {
        guard let descriptor = UIFont.systemFont(ofSize: font.pointSize).fontDescriptor.withSymbolicTraits(traits) else { return font }
        return UIFont(descriptor: descriptor, size: font.pointSize)
    }
}

@MainActor
struct SecureDriveDocumentEditor: View {
    let bytes: [UInt8]
    let readOnly: Bool
    let onEdit: ([UInt8]) -> Void
    @State private var controller = SecureDriveRichTextController()

    var body: some View {
        VStack(spacing: 0) {
            if !readOnly {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 4) {
                        Menu {
                            Button("Body text") { controller.setLevel(0) }
                            Button("Heading") { controller.setLevel(1) }
                            Button("Subheading") { controller.setLevel(2) }
                        } label: {
                            Label("Style", systemImage: "textformat.size")
                        }
                        .buttonStyle(.bordered)
                        tool("bold", "Bold") { controller.toggle(.traitBold) }
                        tool("italic", "Italic") { controller.toggle(.traitItalic) }
                        tool("underline", "Underline") { controller.toggleUnderline() }
                        tool("list.bullet", "Bulleted list") { controller.toggleList() }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                }
                Divider()
            }
            SecureDriveRichTextView(bytes: bytes, readOnly: readOnly, controller: controller, onEdit: onEdit)
        }
    }

    private func tool(_ symbol: String, _ label: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).frame(width: 28, height: 22)
        }
        .buttonStyle(.bordered)
        .accessibilityLabel(label)
    }
}

@MainActor
struct SecureDriveRichTextView: UIViewRepresentable {
    let bytes: [UInt8]
    let readOnly: Bool
    let controller: SecureDriveRichTextController
    let onEdit: ([UInt8]) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(controller: controller) }

    func makeUIView(context: Context) -> UITextView {
        let view = UITextView()
        view.isEditable = !readOnly
        view.alwaysBounceVertical = true
        view.textContainerInset = UIEdgeInsets(top: 16, left: 12, bottom: 16, right: 12)
        view.font = UIFont.systemFont(ofSize: SecureDriveRichTextController.bodySize)
        view.attributedText = SecureDriveHTML.read(bytes)
        view.typingAttributes = [.font: UIFont.systemFont(ofSize: SecureDriveRichTextController.bodySize), .foregroundColor: UIColor.label]
        view.delegate = context.coordinator
        controller.view = view
        controller.onEdit = onEdit
        return view
    }

    func updateUIView(_ view: UITextView, context: Context) {
        controller.onEdit = onEdit
    }

    final class Coordinator: NSObject, UITextViewDelegate {
        let controller: SecureDriveRichTextController

        init(controller: SecureDriveRichTextController) {
            self.controller = controller
        }

        func textViewDidChange(_ textView: UITextView) {
            MainActor.assumeIsolated { controller.changed() }
        }
    }
}

enum SecureDriveHTML {
    static func bulletLength(_ text: NSString, at index: Int) -> Int {
        let remaining = text.length - index
        if remaining >= 2 && text.substring(with: NSRange(location: index, length: 2)) == "\u{2022} " { return 2 }
        if remaining >= 3 && text.substring(with: NSRange(location: index, length: 3)) == "\t\u{2022}\t" { return 3 }
        return 0
    }

    @MainActor
    static func read(_ bytes: [UInt8]) -> NSAttributedString {
        let body = UIFont.systemFont(ofSize: SecureDriveRichTextController.bodySize)
        guard !bytes.isEmpty,
              let imported = try? NSAttributedString(
                data: Data(bytes),
                options: [.documentType: NSAttributedString.DocumentType.html, .characterEncoding: String.Encoding.utf8.rawValue],
                documentAttributes: nil) else {
            return NSAttributedString(string: "", attributes: [.font: body, .foregroundColor: UIColor.label])
        }
        let text = NSMutableAttributedString(attributedString: imported)
        while text.length > 0 && text.string.hasSuffix("\n") {
            text.deleteCharacters(in: NSRange(location: text.length - 1, length: 1))
        }
        let whole = NSRange(location: 0, length: text.length)
        text.removeAttribute(.foregroundColor, range: whole)
        text.removeAttribute(.backgroundColor, range: whole)
        text.removeAttribute(.paragraphStyle, range: whole)
        text.addAttribute(.foregroundColor, value: UIColor.label, range: whole)
        text.enumerateAttribute(.font, in: whole) { value, range, _ in
            let font = (value as? UIFont) ?? body
            let ratio = font.pointSize / 12
            let size: CGFloat = ratio >= 1.8 ? SecureDriveRichTextController.h1Size : ratio >= 1.3 ? SecureDriveRichTextController.h2Size : SecureDriveRichTextController.bodySize
            var traits: UIFontDescriptor.SymbolicTraits = []
            let source = font.fontDescriptor.symbolicTraits
            if source.contains(.traitBold) { traits.insert(.traitBold) }
            if source.contains(.traitItalic) { traits.insert(.traitItalic) }
            text.addAttribute(.font, value: SecureDriveRichTextController.font(UIFont.systemFont(ofSize: size), traits: traits), range: range)
        }
        let string = text.string as NSString
        var starts = [Int]()
        string.enumerateSubstrings(in: NSRange(location: 0, length: string.length), options: [.byParagraphs, .substringNotRequired]) { _, range, _, _ in
            starts.append(range.location)
        }
        for start in starts.reversed() where bulletLength(text.string as NSString, at: start) == 3 {
            text.replaceCharacters(in: NSRange(location: start, length: 3), with: "\u{2022} ")
        }
        return text
    }

    static func escape(_ text: String) -> String {
        text.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;").replacingOccurrences(of: "\"", with: "&quot;")
    }

    static func write(_ text: NSAttributedString) -> String {
        let string = text.string as NSString
        var out = "<html><body>"
        var inList = false
        var paragraphs = [NSRange]()
        string.enumerateSubstrings(in: NSRange(location: 0, length: string.length), options: [.byParagraphs, .substringNotRequired]) { _, range, _, _ in
            paragraphs.append(range)
        }
        if paragraphs.isEmpty { paragraphs = [NSRange(location: 0, length: 0)] }
        for paragraph in paragraphs {
            let bullet = bulletLength(string, at: paragraph.location)
            let content = NSRange(location: paragraph.location + bullet, length: max(0, paragraph.length - bullet))
            var largest: CGFloat = 0
            if content.length > 0 {
                text.enumerateAttribute(.font, in: content) { value, _, _ in
                    largest = max(largest, (value as? UIFont)?.pointSize ?? SecureDriveRichTextController.bodySize)
                }
            }
            let tag = bullet > 0 ? "li" : largest >= SecureDriveRichTextController.h1Size - 1 ? "h1" : largest >= SecureDriveRichTextController.h2Size - 1 ? "h2" : "p"
            if tag == "li" && !inList {
                out += "<ul>"
                inList = true
            } else if tag != "li" && inList {
                out += "</ul>"
                inList = false
            }
            out += "<\(tag)>" + runs(text, content) + "</\(tag)>"
        }
        if inList { out += "</ul>" }
        return out + "</body></html>"
    }

    static func runs(_ text: NSAttributedString, _ range: NSRange) -> String {
        guard range.length > 0 else { return "" }
        var out = ""
        text.enumerateAttributes(in: range) { attributes, sub, _ in
            let piece = escape((text.string as NSString).substring(with: sub)).replacingOccurrences(of: "\u{2028}", with: "<br>")
            let font = attributes[.font] as? UIFont
            let traits = font?.fontDescriptor.symbolicTraits ?? []
            let heading = (font?.pointSize ?? 0) >= SecureDriveRichTextController.h2Size - 1
            let bold = traits.contains(.traitBold) && !heading
            let italic = traits.contains(.traitItalic)
            let underline = (attributes[.underlineStyle] as? Int ?? 0) != 0
            var run = piece
            if underline { run = "<u>\(run)</u>" }
            if italic { run = "<i>\(run)</i>" }
            if bold { run = "<b>\(run)</b>" }
            out += run
        }
        return out
    }
}
