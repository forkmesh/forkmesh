import FMApi

extension UsageWindowMeter {
    public init(runner window: RunnerUsageWindow) {
        let used: Int = Int(window.usedPercent.rounded())
        self.init(
            id: window.id, label: window.label, usedPercent: used,
            resetAtMs: window.resetsAtMs > 0 ? window.resetsAtMs : nil,
            state: used >= 90 ? .exhausted : used >= 70 ? .high : .ok)
    }
}
