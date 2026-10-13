@testable import FMAgentWatch

/// A golden note as the generator writes it: start, frequency, seconds, gain,
/// pan, timbre index, instrument index, bloom, seed.
struct N {
    let note: Note

    init(
        _ start: Double, _ frequency: Double, _ seconds: Double, _ gain: Double, _ pan: Double, _ timbre: Int,
        _ instrument: Int, _ bloom: Double, _ seed: Int
    ) {
        note = Note(
            start: start, frequency: frequency, seconds: seconds, gain: gain, pan: pan,
            timbre: Timbre(rawValue: timbre)!, instrument: Instrument(rawValue: instrument)!, bloom: bloom,
            seed: seed)
    }
}

func motionNamed(_ name: String) -> AgentMotion {
    AgentMotion.allCases.first { $0.name == name }!
}
