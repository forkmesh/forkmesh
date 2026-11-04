import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class SurveysModel {
    public private(set) var organizations: [PackageOrganization] = []
    public private(set) var board: SurveyBoard?
    public private(set) var loading = false
    public private(set) var working = false
    public private(set) var loaded = false
    public var organization: String
    public var errorText = ""
    public var notice = ""

    @ObservationIgnored public let client: PackagesClient

    public init(client: PackagesClient, organization: String = "") {
        self.client = client
        self.organization = organization
    }

    public var selected: PackageOrganization? { organizations.first { $0.name == organization } }

    public var needsInstall: Bool { selected.map { !$0.installed } ?? false }

    public var canManage: Bool { board?.canManage ?? selected?.canManage ?? false }

    public func refresh() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            organizations = try await client.surveyOrganizations()
            errorText = ""
        } catch {
            errorText = SurveyText.describe(error)
            return
        }
        if organization.isEmpty || selected == nil {
            organization = organizations.first(where: \.installed)?.name ?? organizations.first?.name ?? ""
        }
        await load()
    }

    public func select(_ name: String) async {
        guard name != organization else { return }
        organization = name
        await organizationChanged()
    }

    public func organizationChanged() async {
        board = nil
        await load()
    }

    public func locate(_ surveyId: String) async -> Survey? {
        guard PackagesText.isHexId(surveyId) else { return nil }
        if board != nil, let found = try? await detail(surveyId) { return found }
        for org in organizations where org.installed && org.name != organization {
            if let found = try? await client.survey(org.name, id: surveyId) {
                organization = org.name
                await organizationChanged()
                return found
            }
        }
        return nil
    }

    public func load() async {
        guard PackagesText.isOrgName(organization), !needsInstall else {
            board = nil
            return
        }
        do {
            board = try await client.surveys(organization)
            errorText = ""
        } catch let error as PackagesError where error.code == "plugin_not_installed" {
            markInstalled(false)
            board = nil
        } catch {
            errorText = SurveyText.describe(error)
        }
    }

    private func markInstalled(_ installed: Bool) {
        guard let index = organizations.firstIndex(where: { $0.name == organization }) else { return }
        organizations[index].installed = installed
    }

    @discardableResult
    public func install() async -> Bool {
        guard canManage || selected?.canManage == true else { return false }
        working = true
        defer { working = false }
        do {
            _ = try await client.installSurveys(organization)
            markInstalled(true)
            notice = "Surveys installed."
            errorText = ""
            await load()
            return true
        } catch {
            errorText = "Could not install: " + SurveyText.describe(error)
            return false
        }
    }

    public func detail(_ id: String) async throws -> Survey {
        let survey = try await client.survey(organization, id: id)
        board?.replace(survey)
        return survey
    }

    public func newDraft() -> SurveyDraft { SurveyDraft() }

    public func save(_ draft: SurveyDraft) async throws -> Survey {
        let saved = try await perform { try await self.client.saveSurvey(self.organization, draft) }
        notice = draft.isNew ? "Survey \(saved.displayTitle) created as a draft." : "Survey \(saved.displayTitle) saved."
        return saved
    }

    public func open(_ survey: Survey) async throws -> Survey {
        let opened = try await perform { try await self.client.openSurvey(self.organization, id: survey.id) }
        notice = survey.isDraft ? "\(opened.displayTitle) is open for answers." : "\(opened.displayTitle) reopened."
        return opened
    }

    public func close(_ survey: Survey) async throws -> Survey {
        let closed = try await perform { try await self.client.closeSurvey(self.organization, id: survey.id) }
        notice = "\(closed.displayTitle) is closed."
        return closed
    }

    public func duplicate(_ survey: Survey) async throws -> Survey {
        let copy = try await perform { try await self.client.duplicateSurvey(self.organization, id: survey.id) }
        notice = "Made a draft copy: \(copy.displayTitle)."
        return copy
    }

    public func delete(_ survey: Survey) async throws {
        working = true
        defer { working = false }
        try await client.deleteSurvey(organization, id: survey.id)
        board?.remove(survey.id)
        notice = "Survey \(survey.displayTitle) deleted."
        await load()
    }

    public func responses(_ survey: Survey) async throws -> [SurveyResponse] {
        try await client.surveyResponses(organization, id: survey.id)
    }

    public func deleteResponse(_ response: SurveyResponse, of survey: Survey) async throws -> Survey {
        working = true
        defer { working = false }
        let updated = try await client.deleteSurveyResponse(organization, id: survey.id, response: response.id)
        if PackagesText.isHexId(updated.id) { board?.replace(updated) }
        notice = "Response deleted."
        return updated
    }

    public func export(_ survey: Survey) async throws -> SurveyExport {
        working = true
        defer { working = false }
        return try await client.exportSurvey(organization, id: survey.id)
    }

    private func perform(_ action: @MainActor () async throws -> Survey) async throws -> Survey {
        working = true
        defer { working = false }
        let survey = try await action()
        board?.replace(survey)
        await load()
        return survey
    }
}
