import FMBase
import Foundation

public struct CovePipelineStage: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var color: String
}

public struct CovePipelineLead: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var company: String
    public var value: Double
    public var owner: String
    public var stage: String
    public var updatedAt: String

    public init(
        id: String, name: String, company: String = "", value: Double = 0, owner: String = "", stage: String,
        updatedAt: String = ""
    ) {
        self.id = id
        self.name = name
        self.company = company
        self.value = value
        self.owner = owner
        self.stage = stage
        self.updatedAt = updatedAt
    }
}

public struct CovePipelineTotal: Equatable, Sendable, Identifiable {
    public var stage: CovePipelineStage
    public var count: Int
    public var value: Double

    public var id: String { stage.id }
}

public struct CovePipeline: Equatable, Sendable {
    public var root: JSONObject
    public var stages: [CovePipelineStage]
    public var leads: [CovePipelineLead]
    public var history: [JSONValue]

    public enum ParseError: Error, Equatable, CustomStringConvertible {
        case notObject
        case invalidJson
        case noStages
        case badStage
        case badArrays

        public var description: String {
            switch self {
            case .notObject: return "The pipeline root must be a JSON object."
            case .invalidJson: return "Invalid pipeline JSON."
            case .noStages: return "A pipeline needs at least one stage."
            case .badStage: return "Every pipeline stage needs a unique id and name."
            case .badArrays: return "Pipeline leads and history must be JSON arrays."
            }
        }
    }

    public static func parse(_ body: String) -> Result<CovePipeline, ParseError> {
        let value: JSONValue
        do {
            value = try JSON.parse(body)
        } catch {
            return .failure(.invalidJson)
        }
        guard var object = value.objectValue else { return .failure(.notObject) }
        let rawStages = object["stages"]?.arrayValue ?? []
        if rawStages.isEmpty { return .failure(.noStages) }
        var seen = Set<String>()
        var stages: [CovePipelineStage] = []
        for raw in rawStages {
            let stage = raw.objectValue ?? JSONObject()
            let id = (stage["id"]?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            let name = (stage["name"]?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if id.isEmpty || name.isEmpty || seen.contains(id) { return .failure(.badStage) }
            seen.insert(id)
            stages.append(CovePipelineStage(id: id, name: name, color: stage["color"]?.stringValue ?? ""))
        }
        if let leads = object["leads"], leads.arrayValue == nil { return .failure(.badArrays) }
        if let history = object["history"], history.arrayValue == nil { return .failure(.badArrays) }
        if object["leads"] == nil { object["leads"] = .array([]) }
        if object["history"] == nil { object["history"] = .array([]) }
        if object["version"] == nil { object["version"] = .int(1) }
        let leads = (object["leads"]?.arrayValue ?? []).compactMap(\.objectValue).map { lead in
            CovePipelineLead(
                id: lead["id"]?.stringValue ?? "", name: lead["name"]?.stringValue ?? "",
                company: lead["company"]?.stringValue ?? "", value: lead["value"].map { $0.double(or: 0) } ?? 0,
                owner: lead["owner"]?.stringValue ?? "", stage: lead["stage"]?.stringValue ?? "",
                updatedAt: lead["updatedAt"]?.stringValue ?? "")
        }
        return .success(CovePipeline(root: object, stages: stages, leads: leads, history: object["history"]?.arrayValue ?? []))
    }

    public func stageName(_ id: String) -> String { stages.first { $0.id == id }?.name ?? id }

    public var body: String {
        var root = self.root
        root["stages"] = .array(stages.map { .object(["color": .string($0.color), "id": .string($0.id), "name": .string($0.name)]) })
        root["leads"] = .array(leads.map {
            .object([
                "company": .string($0.company), "id": .string($0.id), "name": .string($0.name), "owner": .string($0.owner),
                "stage": .string($0.stage), "updatedAt": .string($0.updatedAt), "value": .double($0.value),
            ])
        })
        root["history"] = .array(history)
        root["version"] = .int(1)
        return JSON.encodePretty(.object(root), indent: "    ") + "\n"
    }

    private mutating func record(_ lead: CovePipelineLead, from: String, to: String, actor: String, action: String, now: Date) {
        history.append(.object([
            "action": .string(action), "at": .string(CoveFiles.isoNow(now)), "by": .string(actor), "from": .string(from),
            "lead": .string(lead.name.trimmingCharacters(in: .whitespacesAndNewlines)), "leadId": .string(lead.id),
            "to": .string(to),
        ]))
    }

    @discardableResult
    public mutating func addLead(name: String = "New lead", actor: String, now: Date = Date(), id: String = CoveStore.newId()) -> CovePipelineLead {
        let stage = stages.first?.id ?? ""
        let lead = CovePipelineLead(id: id, name: name, value: 0, owner: actor, stage: stage, updatedAt: CoveFiles.isoNow(now))
        leads.append(lead)
        record(lead, from: "", to: stage, actor: actor, action: "created", now: now)
        return lead
    }

    public mutating func move(leadId: String, to stage: String, actor: String, now: Date = Date()) {
        guard let index = leads.firstIndex(where: { $0.id == leadId }), stages.contains(where: { $0.id == stage }) else { return }
        let from = leads[index].stage
        if from == stage { return }
        leads[index].stage = stage
        leads[index].updatedAt = CoveFiles.isoNow(now)
        record(leads[index], from: from, to: stage, actor: actor, action: "moved", now: now)
    }

    public mutating func update(_ lead: CovePipelineLead, now: Date = Date()) {
        guard let index = leads.firstIndex(where: { $0.id == lead.id }) else { return }
        var updated = lead
        updated.stage = leads[index].stage
        updated.updatedAt = CoveFiles.isoNow(now)
        leads[index] = updated
    }

    public mutating func delete(leadId: String, actor: String, now: Date = Date()) {
        guard let index = leads.firstIndex(where: { $0.id == leadId }) else { return }
        let lead = leads.remove(at: index)
        record(lead, from: lead.stage, to: "", actor: actor, action: "deleted", now: now)
    }

    public func totals() -> [CovePipelineTotal] {
        stages.map { stage in
            let inStage = leads.filter { $0.stage == stage.id }
            return CovePipelineTotal(stage: stage, count: inStage.count, value: inStage.reduce(0) { $0 + $1.value })
        }
    }
}
