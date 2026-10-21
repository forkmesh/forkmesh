import FMBase
import Foundation
import UIKit

@MainActor
enum HiddenUnicodeInputGuard {
    private static var installed = false

    static func install() {
        guard !installed else { return }
        installed = true
        let center = NotificationCenter.default
        center.addObserver(forName: UITextField.textDidChangeNotification, object: nil, queue: .main) { note in
            let field = note.object as? UITextField
            MainActor.assumeIsolated {
                if let field { clean(field) }
            }
        }
        center.addObserver(forName: UITextView.textDidChangeNotification, object: nil, queue: .main) { note in
            let view = note.object as? UITextView
            MainActor.assumeIsolated {
                if let view { clean(view) }
            }
        }
    }

    static func clean(_ field: UITextField) {
        guard !field.isSecureTextEntry, field.markedTextRange == nil, let text = field.text else { return }
        let cleaned = HiddenUnicode.clean(text)
        guard cleaned != text else { return }
        let edits = diff(from: text, to: cleaned)
        let caret = field.selectedTextRange.map { field.offset(from: field.beginningOfDocument, to: $0.end) }
        field.text = cleaned
        if let caret, let position = field.position(from: field.beginningOfDocument, offset: shifted(caret, by: edits)) {
            field.selectedTextRange = field.textRange(from: position, to: position)
        }
        field.sendActions(for: .editingChanged)
    }

    static func clean(_ view: UITextView) {
        guard view.isEditable, !view.isSecureTextEntry, view.markedTextRange == nil else { return }
        let text = view.textStorage.string
        let cleaned = HiddenUnicode.clean(text)
        guard cleaned != text else { return }
        let edits = diff(from: text, to: cleaned)
        let caret = view.selectedRange.location + view.selectedRange.length
        view.textStorage.beginEditing()
        for edit in edits.reversed() {
            view.textStorage.replaceCharacters(in: edit.range, with: edit.replacement)
        }
        view.textStorage.endEditing()
        view.selectedRange = NSRange(location: min(shifted(caret, by: edits), (cleaned as NSString).length), length: 0)
        view.delegate?.textViewDidChange?(view)
    }

    private struct Edit {
        let range: NSRange
        let replacement: String
    }

    private static func diff(from original: String, to cleaned: String) -> [Edit] {
        var edits: [Edit] = []
        var kept = cleaned.unicodeScalars.makeIterator()
        var next = kept.next()
        var offset = 0
        for scalar in original.unicodeScalars {
            let width = scalar.utf16.count
            if let candidate = next, candidate == scalar {
                next = kept.next()
            } else if let candidate = next, candidate == "\n", scalar.value == 0x2028 || scalar.value == 0x2029 {
                edits.append(Edit(range: NSRange(location: offset, length: width), replacement: "\n"))
                next = kept.next()
            } else {
                edits.append(Edit(range: NSRange(location: offset, length: width), replacement: ""))
            }
            offset += width
        }
        return edits
    }

    private static func shifted(_ caret: Int, by edits: [Edit]) -> Int {
        var position = caret
        for edit in edits where edit.range.location < caret {
            let removed = min(edit.range.length, caret - edit.range.location)
            position -= removed - (edit.replacement.isEmpty ? 0 : min(1, removed))
        }
        return max(0, position)
    }
}
