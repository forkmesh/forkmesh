// The code editor: a line-number gutter beside a syntax-coloured, non-wrapping
// buffer that scrolls both ways, with the caret's line highlighted across the
// full width, auto-indent, and CodeKeyBar (Tab, the symbols phone keyboards
// bury, undo, redo, hide keyboard) docked above the keyboard.
//
// SwiftUI's TextEditor cannot colour its text, so this is a UITextView. The
// pure parts (caret, indent unit, the Return formatter, the symbols) are
// FMCode's CodeEditing; the colours are FMDiff's CodeHighlighter tokens.
//
// Use: CodeEditorView(text: $draft, language: CodeLang.forPath(path),
//      indentUnit: CodeEditing.detectIndentUnit(original), caret: $caret, focused: $editing)

import FMCode
import FMDiff
import SwiftUI
import UIKit

struct CodeEditorView: UIViewRepresentable {
    @Environment(\.fmPalette) private var fm

    @Binding var text: String
    let language: CodeLang
    let indentUnit: String
    let caret: Binding<CodeCaret>?
    let focused: Binding<Bool>?
    let showsKeyBar: Bool

    /// FmCodeEditor.fontSize and lineHeightFactor.
    static let fontSize: CGFloat = FmType.small
    static let lineHeightFactor: CGFloat = 1.55

    init(
        text: Binding<String>,
        language: CodeLang,
        indentUnit: String = "  ",
        caret: Binding<CodeCaret>? = nil,
        focused: Binding<Bool>? = nil,
        showsKeyBar: Bool = true
    ) {
        self._text = text
        self.language = language
        self.indentUnit = indentUnit
        self.caret = caret
        self.focused = focused
        self.showsKeyBar = showsKeyBar
    }

    func makeCoordinator() -> CodeEditorCoordinator {
        return CodeEditorCoordinator(parent: self)
    }

    func makeUIView(context: Context) -> CodeEditorTextView {
        let view: CodeEditorTextView = CodeEditorTextView.make()
        let coordinator: CodeEditorCoordinator = context.coordinator
        coordinator.parent = self
        coordinator.textView = view
        view.delegate = coordinator
        view.configure(palette: CodeEditorPalette(fm))
        view.text = text
        if showsKeyBar {
            view.inputAccessoryView = CodeKeyBar(coordinator: coordinator, palette: CodeEditorPalette(fm))
        }
        coordinator.restyle()
        return view
    }

    func updateUIView(_ view: CodeEditorTextView, context: Context) {
        let coordinator: CodeEditorCoordinator = context.coordinator
        let languageChanged: Bool = coordinator.parent.language != language
        coordinator.parent = self
        let palette = CodeEditorPalette(fm)
        let paletteChanged: Bool = view.palette != palette
        if paletteChanged {
            view.configure(palette: palette)
            (view.inputAccessoryView as? CodeKeyBar)?.apply(palette)
        }
        if view.text != text {
            view.text = text
            coordinator.restyle()
        } else if paletteChanged || languageChanged {
            coordinator.restyle()
        }
        if let focused = focused {
            if focused.wrappedValue && !view.isFirstResponder {
                DispatchQueue.main.async { view.becomeFirstResponder() }
            } else if !focused.wrappedValue && view.isFirstResponder {
                DispatchQueue.main.async { view.resignFirstResponder() }
            }
        }
    }
}

/// The FmTheme colours the editor draws with, as UIKit colours.
struct CodeEditorPalette: Equatable {
    let dark: Bool
    let surface: UIColor
    let gutter: UIColor
    let border: UIColor
    let activeLine: UIColor
    let numbers: UIColor
    let activeNumber: UIColor
    let caret: UIColor
    let keyBackground: UIColor
    let keyText: UIColor
    let barBackground: UIColor
    let disabled: UIColor
    private let tokenColors: [String: UIColor]

    init(_ fm: FmPalette) {
        dark = fm.dark
        surface = UIColor(fm.readingSurface)
        gutter = UIColor(fm.readingGutter)
        border = UIColor(fm.border)
        activeLine = UIColor(fm.editorActiveLine)
        numbers = UIColor(fm.textDisabled)
        activeNumber = UIColor(fm.textSecondary)
        caret = UIColor(fm.accent)
        keyBackground = UIColor(fm.bgOverlay)
        keyText = UIColor(fm.textPrimary)
        barBackground = UIColor(fm.bgRaised)
        disabled = UIColor(fm.textDisabled)
        tokenColors = [
            "codeBase": UIColor(RepoCodeColors.base(fm)),
            "codeKeyword": UIColor(fm.codeKeyword),
            "codeType": UIColor(fm.codeType),
            "codeString": UIColor(fm.codeString),
            "codeComment": UIColor(fm.codeComment),
            "codeNumber": UIColor(fm.codeNumber),
            "codeFunction": UIColor(fm.codeFunction),
        ]
    }

    var base: UIColor {
        return tokenColors["codeBase"] ?? UIColor.label
    }

    func color(_ kind: TokenKind) -> UIColor {
        return tokenColors[kind.themeColorName] ?? base
    }

    static func == (lhs: CodeEditorPalette, rhs: CodeEditorPalette) -> Bool {
        return lhs.dark == rhs.dark && lhs.surface == rhs.surface && lhs.caret == rhs.caret
            && lhs.base == rhs.base
    }
}

/// The buffer: a non-wrapping UITextView with the gutter and the current-line
/// band as subviews, both on one fixed line grid.
final class CodeEditorTextView: UITextView {
    static let padTop: CGFloat = FmSpace.x2
    static let padBottom: CGFloat = FmSpace.x6
    static let gutterPad: CGFloat = FmSpace.x3

    private(set) var palette: CodeEditorPalette?
    /// TextKit 1 storage, kept alive here (the layout manager does not own it).
    private var ownedStorage: NSTextStorage?
    let gutter = CodeEditorGutter(frame: CGRect.zero)
    let band = UIView(frame: CGRect.zero)
    var lineCount: Int = 1
    var activeLine: Int = 0
    var showsActiveLine: Bool = false

    var codeFont: UIFont {
        return UIFont(name: "Menlo", size: CodeEditorView.fontSize)
            ?? UIFont.monospacedSystemFont(ofSize: CodeEditorView.fontSize, weight: .regular)
    }

    var lineHeight: CGFloat {
        return (CodeEditorView.fontSize * CodeEditorView.lineHeightFactor).rounded()
    }

    var charWidth: CGFloat {
        let sample: NSString = "0000000000"
        return sample.size(withAttributes: [NSAttributedString.Key.font: codeFont]).width / 10
    }

    var gutterWidth: CGFloat {
        let digits: Int = max(2, String(lineCount).count)
        return CGFloat(digits) * charWidth + CodeEditorTextView.gutterPad * 2
    }

    var paragraphStyle: NSParagraphStyle {
        let style = NSMutableParagraphStyle()
        style.minimumLineHeight = lineHeight
        style.maximumLineHeight = lineHeight
        style.lineBreakMode = .byClipping
        return style
    }

    /// A TextKit 1 stack whose container is as wide as the longest line, so
    /// lines never wrap and the view scrolls sideways.
    static func make() -> CodeEditorTextView {
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let container = NSTextContainer(
            size: CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude))
        container.widthTracksTextView = false
        layout.addTextContainer(container)
        let view = CodeEditorTextView(frame: CGRect.zero, textContainer: container)
        view.ownedStorage = storage
        return view
    }

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        autocapitalizationType = .none
        autocorrectionType = .no
        spellCheckingType = .no
        smartDashesType = .no
        smartQuotesType = .no
        smartInsertDeleteType = .no
        keyboardType = .asciiCapable
        alwaysBounceVertical = true
        // No wrapping: the container is as wide as the longest line.
        self.textContainer.widthTracksTextView = false
        self.textContainer.size = CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        self.textContainer.lineFragmentPadding = 0
        self.textContainer.lineBreakMode = .byClipping
        showsHorizontalScrollIndicator = true
        insertSubview(band, at: 0)
        addSubview(gutter)
        gutter.isUserInteractionEnabled = true
        gutter.onTap = { [weak self] (y: CGFloat) in
            self?.tapGutter(atY: y)
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    func configure(palette: CodeEditorPalette) {
        self.palette = palette
        backgroundColor = palette.surface
        tintColor = palette.caret
        band.backgroundColor = palette.activeLine
        gutter.palette = palette
        keyboardAppearance = palette.dark ? .dark : .light
        font = codeFont
        typingAttributes = [
            NSAttributedString.Key.font: codeFont,
            NSAttributedString.Key.foregroundColor: palette.base,
            NSAttributedString.Key.paragraphStyle: paragraphStyle,
        ]
        layoutGrid()
    }

    /// Recomputes the insets and places the gutter and the band for the
    /// current scroll offset.
    func layoutGrid() {
        let left: CGFloat = gutterWidth + CodeEditorTextView.gutterPad
        let insets = UIEdgeInsets(
            top: CodeEditorTextView.padTop, left: left, bottom: CodeEditorTextView.padBottom, right: FmSpace.x6)
        if textContainerInset != insets {
            textContainerInset = insets
        }
        // The gutter covers the visible height only and draws the lines under
        // it, so a long file never asks for a giant layer.
        gutter.frame = CGRect(x: contentOffset.x, y: contentOffset.y, width: gutterWidth, height: bounds.height)
        gutter.offsetY = contentOffset.y
        gutter.lineHeight = lineHeight
        gutter.lineCount = lineCount
        gutter.activeLine = activeLine
        gutter.showsActive = showsActiveLine
        gutter.font = codeFont
        gutter.setNeedsDisplay()
        band.isHidden = !showsActiveLine
        band.frame = CGRect(
            x: contentOffset.x,
            y: CodeEditorTextView.padTop + CGFloat(activeLine) * lineHeight,
            width: bounds.width,
            height: lineHeight)
        sendSubviewToBack(band)
        bringSubviewToFront(gutter)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        layoutGrid()
    }

    /// Tapping a line number puts the caret at the start of that line.
    private func tapGutter(atY y: CGFloat) {
        let line: Int = min(max(Int(((y - CodeEditorTextView.padTop) / lineHeight).rounded(.down)), 0), max(lineCount - 1, 0))
        let units: [UInt16] = Array((text ?? "").utf16)
        var offset: Int = 0
        var seen: Int = 0
        while seen < line, let next = units[offset...].firstIndex(of: 0x0A) {
            offset = next + 1
            seen += 1
        }
        becomeFirstResponder()
        selectedRange = NSRange(location: offset, length: 0)
    }
}

/// The line numbers, the caret's in the secondary colour.
final class CodeEditorGutter: UIView {
    var palette: CodeEditorPalette?
    var lineHeight: CGFloat = 20
    var lineCount: Int = 1
    var activeLine: Int = 0
    var showsActive: Bool = false
    var font: UIFont = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
    /// The text view's vertical scroll offset: line y positions are in
    /// content coordinates, this view sits at the top of the viewport.
    var offsetY: CGFloat = 0
    var onTap: ((CGFloat) -> Void)?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false
        contentMode = .redraw
        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        addGestureRecognizer(tap)
        isAccessibilityElement = false
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    @objc private func tapped(_ recognizer: UITapGestureRecognizer) {
        onTap?(recognizer.location(in: self).y + offsetY)
    }

    override func draw(_ rect: CGRect) {
        guard let palette = palette else {
            return
        }
        palette.gutter.setFill()
        UIRectFill(bounds)
        palette.border.setFill()
        UIRectFill(CGRect(x: bounds.width - 1, y: 0, width: 1, height: bounds.height))
        let style = NSMutableParagraphStyle()
        style.alignment = .right
        let top: CGFloat = rect.minY + offsetY
        let bottom: CGFloat = rect.maxY + offsetY
        let first: Int = max(0, Int(((top - CodeEditorTextView.padTop) / lineHeight).rounded(.down)))
        let last: Int = min(lineCount - 1, Int(((bottom - CodeEditorTextView.padTop) / lineHeight).rounded(.up)))
        if first > last {
            return
        }
        for line in first...last {
            let active: Bool = showsActive && line == activeLine
            let attributes: [NSAttributedString.Key: Any] = [
                NSAttributedString.Key.font: font,
                NSAttributedString.Key.foregroundColor: active ? palette.activeNumber : palette.numbers,
                NSAttributedString.Key.paragraphStyle: style,
            ]
            let y: CGFloat = CodeEditorTextView.padTop + CGFloat(line) * lineHeight - offsetY
                + (lineHeight - font.lineHeight) / 2
            let box = CGRect(x: 0, y: y, width: bounds.width - CodeEditorTextView.gutterPad, height: font.lineHeight)
            (String(line + 1) as NSString).draw(in: box, withAttributes: attributes)
        }
    }
}

/// UITextView delegate: bindings, highlighting, auto-indent, the key bar.
@MainActor
final class CodeEditorCoordinator: NSObject, UITextViewDelegate {
    var parent: CodeEditorView
    weak var textView: CodeEditorTextView?

    init(parent: CodeEditorView) {
        self.parent = parent
    }

    private var colonOpensBlock: Bool {
        return parent.language == .python || parent.language == .yaml
    }

    // MARK: Highlighting

    /// Re-colours the whole buffer (`CodeEditingController.buildTextSpan`).
    /// Skipped while the keyboard is composing, so the IME's marked text keeps
    /// its underline; colours return with the next committed edit.
    func restyle() {
        guard let view = textView, let palette = view.palette else {
            return
        }
        if view.markedTextRange != nil {
            return
        }
        let text: String = view.text ?? ""
        let storage: NSTextStorage = view.textStorage
        let full = NSRange(location: 0, length: storage.length)
        let selection: NSRange = view.selectedRange
        storage.beginEditing()
        storage.setAttributes([
            NSAttributedString.Key.font: view.codeFont,
            NSAttributedString.Key.foregroundColor: palette.base,
            NSAttributedString.Key.paragraphStyle: view.paragraphStyle,
        ], range: full)
        if parent.language != .plain {
            let highlighter = CodeHighlighter(parent.language)
            let lines: [String] = text.components(separatedBy: "\n")
            let inBlock: [Bool] = highlighter.blockCommentStarts(lines)
            var lineStart: Int = 0
            for (index, line) in lines.enumerated() {
                let tokens: [HighlightToken] = highlighter.highlight(line, startsInBlockComment: inBlock[index])
                for token in tokens where token.kind != .plain {
                    let range = NSRange(location: lineStart + token.start, length: token.end - token.start)
                    if range.location + range.length > storage.length {
                        continue
                    }
                    storage.addAttribute(NSAttributedString.Key.foregroundColor, value: palette.color(token.kind), range: range)
                    if token.kind.isItalic {
                        let italic: UIFont = UIFont(
                            descriptor: view.codeFont.fontDescriptor.withSymbolicTraits(.traitItalic) ?? view.codeFont.fontDescriptor,
                            size: CodeEditorView.fontSize)
                        storage.addAttribute(NSAttributedString.Key.font, value: italic, range: range)
                    } else if token.kind.isEmphasized {
                        let bold: UIFont = UIFont(
                            descriptor: view.codeFont.fontDescriptor.withSymbolicTraits(.traitBold) ?? view.codeFont.fontDescriptor,
                            size: CodeEditorView.fontSize)
                        storage.addAttribute(NSAttributedString.Key.font, value: bold, range: range)
                    }
                }
                lineStart += line.utf16.count + 1
            }
        }
        storage.endEditing()
        view.selectedRange = selection
        syncCaret()
    }

    /// Caret, line count and the current-line band.
    func syncCaret() {
        guard let view = textView else {
            return
        }
        let text: String = view.text ?? ""
        let range: NSRange = view.selectedRange
        let caret: CodeCaret = CodeEditing.caret(in: text, utf16Offset: range.location + range.length)
        view.lineCount = caret.lineCount
        view.activeLine = caret.line - 1
        view.showsActiveLine = view.isFirstResponder
        view.layoutGrid()
        if let binding = parent.caret, binding.wrappedValue != caret {
            binding.wrappedValue = caret
        }
        (view.inputAccessoryView as? CodeKeyBar)?.refreshUndo(view.undoManager)
    }

    // MARK: UITextViewDelegate

    func textView(_ textView: UITextView, shouldChangeTextIn range: NSRange, replacementText replacement: String) -> Bool {
        guard replacement == "\n" else {
            return true
        }
        return !formatReturn(in: textView, range: range)
    }

    /// CodeIndentFormatter on Return: true when it took the edit over.
    private func formatReturn(in textView: UITextView, range: NSRange) -> Bool {
        let old: String = textView.text ?? ""
        let units: [UInt16] = Array(old.utf16)
        let start: Int = range.location
        let end: Int = range.location + range.length
        if end > units.count {
            return false
        }
        let typed: String = String(decoding: Array(units[0..<start]) + [0x0A] + Array(units[end...]), as: UTF16.self)
        let formatted: CodeEditValue = CodeEditing.formatNewline(
            old: CodeEditValue(text: old, selectionStart: start, selectionEnd: end),
            new: CodeEditValue(text: typed, caret: start + 1),
            indentUnit: parent.indentUnit,
            colonOpensBlock: colonOpensBlock)
        if formatted.text == typed {
            return false
        }
        // The inserted run is what the formatter put between the kept prefix
        // and suffix.
        let formattedUnits: [UInt16] = Array(formatted.text.utf16)
        let suffixCount: Int = units.count - end
        let insertUnits: [UInt16] = Array(formattedUnits[start..<(formattedUnits.count - suffixCount)])
        let insert: String = String(decoding: insertUnits, as: UTF16.self)
        replace(range: range, with: insert, caret: formatted.selectionStart)
        return true
    }

    func textViewDidChange(_ textView: UITextView) {
        let value: String = textView.text ?? ""
        if parent.text != value {
            parent.text = value
        }
        restyle()
    }

    func textViewDidChangeSelection(_ textView: UITextView) {
        syncCaret()
    }

    func textViewDidBeginEditing(_ textView: UITextView) {
        if let focused = parent.focused, !focused.wrappedValue {
            focused.wrappedValue = true
        }
        syncCaret()
    }

    func textViewDidEndEditing(_ textView: UITextView) {
        if let focused = parent.focused, focused.wrappedValue {
            focused.wrappedValue = false
        }
        syncCaret()
    }

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        textView?.layoutGrid()
    }

    // MARK: Edits

    /// Replaces `range` as a user edit (it lands on the undo stack) and puts
    /// the caret at `caret`.
    func replace(range: NSRange, with insert: String, caret: Int) {
        guard let view = textView,
              let from = view.position(from: view.beginningOfDocument, offset: range.location),
              let to = view.position(from: from, offset: range.length),
              let textRange = view.textRange(from: from, to: to)
        else {
            return
        }
        view.replace(textRange, withText: insert)
        view.selectedRange = NSRange(location: caret, length: 0)
        textViewDidChange(view)
        view.scrollRangeToVisible(view.selectedRange)
    }

    /// `insertIntoCodeEditor`: replaces the selection with `text`, collapsing
    /// onto the end so the caret lands after the insert.
    func insert(_ text: String) {
        guard let view = textView else {
            return
        }
        let range: NSRange = view.selectedRange
        if text == "\n" && formatReturn(in: view, range: range) {
            return
        }
        replace(range: range, with: text, caret: range.location + text.utf16.count)
    }

    func undo() {
        textView?.undoManager?.undo()
        if let view = textView {
            textViewDidChange(view)
        }
    }

    func redo() {
        textView?.undoManager?.redo()
        if let view = textView {
            textViewDidChange(view)
        }
    }

    func hideKeyboard() {
        textView?.resignFirstResponder()
    }
}

/// CodeKeyBar: Tab, the symbol row, undo / redo and hide keyboard, docked
/// above the software keyboard. Its buttons never take focus from the editor.
final class CodeKeyBar: UIInputView {
    private weak var coordinator: CodeEditorCoordinator?
    private let scroller = UIScrollView(frame: CGRect.zero)
    private let row = UIStackView(frame: CGRect.zero)
    private let undoButton = UIButton(type: .system)
    private let redoButton = UIButton(type: .system)
    private let hideButton = UIButton(type: .system)
    private let topRule = UIView(frame: CGRect.zero)
    private var keys: [UIButton] = []

    init(coordinator: CodeEditorCoordinator, palette: CodeEditorPalette) {
        self.coordinator = coordinator
        super.init(frame: CGRect(x: 0, y: 0, width: 320, height: 44), inputViewStyle: .default)
        allowsSelfSizing = false
        autoresizingMask = [.flexibleWidth]
        build()
        apply(palette)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var intrinsicContentSize: CGSize {
        return CGSize(width: UIView.noIntrinsicMetric, height: 44)
    }

    private func key(_ title: String, symbol: String?, accessibility: String) -> UIButton {
        let button = UIButton(type: .system)
        var config = UIButton.Configuration.filled()
        config.title = title
        if let symbol = symbol {
            config.image = UIImage(systemName: symbol)
            config.imagePadding = FmSpace.x1
        }
        config.contentInsets = NSDirectionalEdgeInsets(top: 0, leading: FmSpace.x2, bottom: 0, trailing: FmSpace.x2)
        config.cornerStyle = .fixed
        config.background.cornerRadius = FmRadius.sm
        button.configuration = config
        button.accessibilityLabel = accessibility
        button.widthAnchor.constraint(greaterThanOrEqualToConstant: 34).isActive = true
        button.heightAnchor.constraint(equalToConstant: 32).isActive = true
        keys.append(button)
        return button
    }

    private func build() {
        topRule.translatesAutoresizingMaskIntoConstraints = false
        addSubview(topRule)

        let tab: UIButton = key("Tab", symbol: "arrow.right.to.line", accessibility: "Indent")
        tab.addAction(UIAction { [weak self] (_: UIAction) in
            guard let coordinator = self?.coordinator else { return }
            coordinator.insert(coordinator.parent.indentUnit)
        }, for: .touchUpInside)

        row.axis = .horizontal
        row.spacing = 4
        row.alignment = .center
        row.translatesAutoresizingMaskIntoConstraints = false
        for symbol in CodeEditing.keyBarSymbols {
            let button: UIButton = key(symbol, symbol: nil, accessibility: symbol)
            let mono: UIFont = UIFont(name: "Menlo", size: FmType.body) ?? UIFont.monospacedSystemFont(ofSize: FmType.body, weight: .regular)
            var config: UIButton.Configuration = button.configuration ?? UIButton.Configuration.filled()
            config.titleTextAttributesTransformer = UIConfigurationTextAttributesTransformer { (incoming: AttributeContainer) -> AttributeContainer in
                var outgoing: AttributeContainer = incoming
                outgoing.uiKit.font = mono
                return outgoing
            }
            button.configuration = config
            button.addAction(UIAction { [weak self] (_: UIAction) in
                self?.coordinator?.insert(symbol)
            }, for: .touchUpInside)
            row.addArrangedSubview(button)
        }
        scroller.showsHorizontalScrollIndicator = false
        scroller.translatesAutoresizingMaskIntoConstraints = false
        scroller.addSubview(row)

        undoButton.setImage(UIImage(systemName: "arrow.uturn.backward"), for: .normal)
        undoButton.accessibilityLabel = "Undo"
        undoButton.addAction(UIAction { [weak self] (_: UIAction) in self?.coordinator?.undo() }, for: .touchUpInside)
        redoButton.setImage(UIImage(systemName: "arrow.uturn.forward"), for: .normal)
        redoButton.accessibilityLabel = "Redo"
        redoButton.addAction(UIAction { [weak self] (_: UIAction) in self?.coordinator?.redo() }, for: .touchUpInside)
        hideButton.setImage(UIImage(systemName: "keyboard.chevron.compact.down"), for: .normal)
        hideButton.accessibilityLabel = "Hide keyboard"
        hideButton.addAction(UIAction { [weak self] (_: UIAction) in self?.coordinator?.hideKeyboard() }, for: .touchUpInside)

        let trailing = UIStackView(arrangedSubviews: [undoButton, redoButton, hideButton])
        trailing.axis = .horizontal
        trailing.spacing = 0
        trailing.translatesAutoresizingMaskIntoConstraints = false
        for button in [undoButton, redoButton, hideButton] {
            button.widthAnchor.constraint(equalToConstant: 40).isActive = true
        }

        tab.translatesAutoresizingMaskIntoConstraints = false
        addSubview(tab)
        addSubview(scroller)
        addSubview(trailing)

        NSLayoutConstraint.activate([
            topRule.topAnchor.constraint(equalTo: topAnchor),
            topRule.leadingAnchor.constraint(equalTo: leadingAnchor),
            topRule.trailingAnchor.constraint(equalTo: trailingAnchor),
            topRule.heightAnchor.constraint(equalToConstant: 1),
            tab.leadingAnchor.constraint(equalTo: leadingAnchor, constant: FmSpace.x1),
            tab.centerYAnchor.constraint(equalTo: centerYAnchor),
            scroller.leadingAnchor.constraint(equalTo: tab.trailingAnchor, constant: FmSpace.x1),
            scroller.topAnchor.constraint(equalTo: topAnchor),
            scroller.bottomAnchor.constraint(equalTo: bottomAnchor),
            scroller.trailingAnchor.constraint(equalTo: trailing.leadingAnchor),
            trailing.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -FmSpace.x1),
            trailing.centerYAnchor.constraint(equalTo: centerYAnchor),
            row.leadingAnchor.constraint(equalTo: scroller.contentLayoutGuide.leadingAnchor, constant: FmSpace.x1),
            row.trailingAnchor.constraint(equalTo: scroller.contentLayoutGuide.trailingAnchor, constant: -FmSpace.x1),
            row.topAnchor.constraint(equalTo: scroller.contentLayoutGuide.topAnchor),
            row.bottomAnchor.constraint(equalTo: scroller.contentLayoutGuide.bottomAnchor),
            row.heightAnchor.constraint(equalTo: scroller.frameLayoutGuide.heightAnchor),
        ])
    }

    func apply(_ palette: CodeEditorPalette) {
        backgroundColor = palette.barBackground
        topRule.backgroundColor = palette.border
        for button in keys {
            var config: UIButton.Configuration = button.configuration ?? UIButton.Configuration.filled()
            config.baseBackgroundColor = palette.keyBackground
            config.baseForegroundColor = palette.keyText
            button.configuration = config
        }
        for button in [undoButton, redoButton, hideButton] {
            button.tintColor = palette.activeNumber
        }
    }

    func refreshUndo(_ manager: UndoManager?) {
        undoButton.isEnabled = manager?.canUndo ?? false
        redoButton.isEnabled = manager?.canRedo ?? false
    }
}
