import FMApi
import FMBase
import Foundation

public struct PromptTextFile: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let text: String

    public init(id: String = PromptPhotoRules.newId(), name: String, text: String) {
        self.id = id
        self.name = name
        self.text = text
    }

    public var chars: Int { text.utf16.count }
}

public enum PromptTextFiles {
    public static let header = "Uploaded files (sent from the phone that started this run):"
    public static let maxCount = 5
    public static let maxBytes = 32 * 1024

    public static func tooLarge(_ name: String) -> PromptAttachmentError {
        let trimmed = name.dartTrimmed
        return PromptAttachmentError(
            "\(trimmed.isEmpty ? "That file" : trimmed) is larger than 32 KB, more than a prompt can carry.")
    }

    public static func read(name: String, bytes: Data) throws -> PromptTextFile {
        let trimmed = name.dartTrimmed
        let label = trimmed.isEmpty ? "That file" : trimmed
        if bytes.isEmpty {
            throw PromptAttachmentError("\(label) is empty, so there is nothing to send with the prompt.")
        }
        if bytes.count > maxBytes { throw tooLarge(label) }
        guard let text = String(data: bytes, encoding: .utf8), !text.contains("\u{0}") else {
            throw PromptAttachmentError(notText(label))
        }
        var body = text.replacingOccurrences(of: "\r\n", with: "\n")
        if body.hasPrefix("\u{FEFF}") { body.removeFirst() }
        return PromptTextFile(name: label, text: body)
    }

    public static func fold(_ prompt: String, _ files: [PromptTextFile]) -> String {
        let typed = prompt.dartTrimmed
        if files.isEmpty { return typed }
        var parts: [String] = typed.isEmpty ? [] : [typed]
        parts.append(header)
        for file in files {
            parts.append("===== BEGIN FILE \(file.name) =====\n\(file.text)\n===== END FILE \(file.name) =====")
        }
        return parts.joined(separator: "\n\n")
    }

    public static func cost(_ prompt: String, _ files: [PromptTextFile]) -> Int {
        fold(prompt, files).utf16.count
    }

    public static func problem(
        adding file: PromptTextFile, to files: [PromptTextFile], prompt: String,
        budget: Int = ApiService.maxAgentPromptChars
    ) -> String? {
        if files.count >= maxCount {
            return "A prompt can carry \(maxCount) files. Remove one to add another."
        }
        let used = cost(prompt, files + [file])
        if used > budget {
            return "\(file.name) does not fit: a prompt holds \(budget) characters and this would make \(used)."
        }
        return nil
    }

    public static func overBudget(
        _ prompt: String, _ files: [PromptTextFile], budget: Int = ApiService.maxAgentPromptChars
    ) -> String? {
        if files.isEmpty { return nil }
        let used = cost(prompt, files)
        if used <= budget { return nil }
        return "The prompt and its files come to \(used) characters; an agent takes \(budget). Remove a file or shorten the prompt."
    }

    public static func sizeLabel(_ file: PromptTextFile) -> String {
        let chars = file.chars
        return chars == 1 ? "1 character" : "\(chars) characters"
    }

    private static func notText(_ label: String) -> String {
        "\(label) is not a text file. An upload travels inside the prompt, so source, logs, diffs and notes arrive intact; images go through Photo Library or Take Photo."
    }
}
