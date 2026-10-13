import FMBase

public enum PromptFix {
    public static let lead: String = "Please fix"

    public static func prompt(_ typed: String) -> String {
        let task: String = typed.dartTrimmed
        return task.isEmpty ? lead : "\(lead): \(task)"
    }
}
