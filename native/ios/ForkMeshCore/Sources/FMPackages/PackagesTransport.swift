import FMApi
import FMBase
import FMNet
import Foundation

public struct PackagesReply: Sendable {
    public var status: Int
    public var json: JSONObject

    public init(status: Int, json: JSONObject) {
        self.status = status
        self.json = json
    }

    public var ok: Bool { (200...299).contains(status) }
}

public struct PackagesError: Error, Equatable, Sendable {
    public var code: String
    public var status: Int
    public var fields: [String: String]

    public init(_ code: String, status: Int = 0, fields: [String: String] = [:]) {
        self.code = code
        self.status = status
        self.fields = fields
    }

    public var message: String {
        if code == "form_invalid", let message = fields["message"], !message.isEmpty { return message }
        return PackagesError.sentence(code, status: status)
    }

    public static func sentence(_ code: String, status: Int = 0) -> String {
        switch code {
        case "invalid_session", "unauthorized", "sign_in_required": return "Sign in again to continue."
        case "organization_not_found": return "That organization was not found, or you are not a member."
        case "plugin_not_installed": return "This package is not installed for that organization yet."
        case "plugin_not_found": return "This package is not in the Market."
        case "plugin_installs_per_account": return "Each account installs this package for itself."
        case "invalid_places": return "Keep it on at least one place. Remove it to take it off everywhere."
        case "plugin_not_installed_for_account": return "Install this package first."
        case "plugin_not_account_synced": return "This package is not installed per account."
        case "invalid_setting": return "That setting value is not allowed."
        case "status_page_not_installed": return "Install Status Page first."
        case "invalid_name": return "Give the monitor a name of 1 to 40 characters."
        case "invalid_url": return "Use an http or https address with no user name or password, up to 300 characters."
        case "too_many_monitors": return "You have the most monitors allowed. Remove one first."
        case "organization_admin_required": return "Only an owner or admin of the organization can do that."
        case "forbidden", "permission_denied": return "Your account is not allowed to do that."
        case "origin_not_allowed": return "The server refused a request from this app."
        case "email_not_verified": return "Confirm your email address first."
        case "maintainer_cannot_rate": return "Maintainers cannot rate their own package."
        case "package_use_required": return "Install or use the package before rating it."
        case "rating_rate_limited": return "Too many ratings for now. Try again later."
        case "review_required": return "Say what didn't work and how to improve it."
        case "review_invalid": return "Some review fields need more detail."
        case "contact_email_exists": return "Another contact already uses that email address."
        case "name_or_email_required": return "A contact needs a name or an email address."
        case "invalid_email": return "That email address is not valid."
        case "contact_not_found", "competitor_not_found", "content_not_found", "not_found", "monitor_not_found":
            return "It was removed or you no longer have access."
        case "reason_required": return "Give the advertiser a reason."
        case "daily_limit_reached": return "You reached today's outreach limit. Try again tomorrow."
        case "outreach_forbidden": return "Ask an owner or admin to add you to the outreach team."
        case "invalid_recipient": return "That recipient address is not valid."
        case "subject_required", "subject_and_message_required": return "Add a subject and a message."
        case "body_required": return "Write a message first."
        case "send_failed", "mailtrap_send_failed": return "The email provider refused the send."
        case "invalid_sender": return "Set a valid sender address in CRM settings."
        case "merge_needs_two_contacts": return "Pick a different contact to merge."
        case "view_not_found": return "That list was removed or is no longer shared with you."
        case "view_owner_required": return "Only the list's owner or an admin can do that."
        case "view_already_in_organization": return "That list is already in this organization."
        case "invalid_mail_box": return "That mail box doesn't exist."
        case "zoho_not_connected": return "Zoho Mail is not connected."
        case "zoho_one_click_unavailable": return "Zoho one-click connect is not available on this server."
        case "deck_link_not_found": return "That deck link was removed."
        case "member_not_found": return "No organization member has that name."
        case "csv_required": return "The file is empty."
        case "invalid_csv": return "The file is not a readable CSV."
        case "csv_header_required": return "The file has no header row."
        case "too_many_csv_rows": return "The file has more than 2000 rows."
        case "edits_required": return "Select some contacts first."
        case "contact_exists": return "Another contact already uses that email address."
        case "title_required", "experiment_title_required": return "Give it a title."
        case "campaign_name_required", "ad_name_required": return "Give it a name."
        case "keyword_term_required": return "Write the search phrase."
        case "invalid_content_url", "invalid_ad_url", "invalid_campaign_url", "invalid_keyword_url", "invalid_channel_url",
             "invalid_link_url":
            return "Links must start with https://."
        case "invalid_ad_dates", "invalid_campaign_dates", "invalid_experiment_dates": return "The end date is before the start."
        case "invalid_ad_currency", "invalid_campaign_currency": return "Use a three-letter currency code such as USD."
        case "invalid_ad_channel": return "Pick an ad platform."
        case "invalid_utm_value": return "Tags use lowercase letters, digits, dots, dashes or underscores."
        case "invalid_reddit_thread_url": return "Paste a Reddit thread link (https://www.reddit.com/r/…/comments/…)."
        case "invalid_reddit_reply_url", "invalid_reddit_comment_url": return "Paste the link of a comment in that thread."
        case "reddit_reply_not_in_thread", "reddit_comment_not_in_thread": return "That comment is in a different thread."
        case "reddit_reply_checks_required":
            return "Before marking it replied, confirm you read the rules and disclosed who you work for, and add your reply link."
        case "reddit_thread_exists": return "That thread is already on the desk."
        case "invalid_landing_path": return "The landing path is a single page such as / or /pricing."
        case "invalid_subreddit": return "That is not a subreddit name."
        case "too_many_reddit_watch": return "Up to 40 subreddits and 40 searches."
        case "invalid_reddit_query": return "Searches are up to 120 characters."
        case "invalid_reddit_account": return "That is not a Reddit username."
        case "too_many_reddit_accounts": return "Up to 10 Reddit accounts."
        case "invalid_post_url": return "Paste the https link of the post."
        case "post_exists": return "That link is already tracked."
        case "post_channel_required": return "Pick the channel the link was posted on."
        case "marketing_posts_full": return "The posted-link list is full."
        case "keyword_exists": return "That keyword is already tracked."
        case "invalid_keyword_rows": return "No keywords found in that text."
        case "funnel_week_required": return "Pick the week."
        case "x_credentials_required": return "Fill in all four X keys."
        case "x_not_connected": return "Connect an X account in Marketing settings first."
        case "x_post_text_required": return "Write the post first."
        case "x_post_too_long": return "The post is too long."
        case "ready_post_changed": return "Herald replaced that post with a newer one."
        case "ready_post_not_found": return "That post was already posted or skipped."
        case "x_account_not_found": return "X did not recognise those keys."
        case "x_access_token_required", "x_access_token_invalid": return "Add an X API bearer token to this channel."
        case "github_rate_limited": return "GitHub is rate limiting; add an access token to this channel."
        case "channel_refresh_unsupported": return "This channel's numbers are logged by hand or with the browser extension."
        case "channel_url_required": return "Add the channel's link first."
        case "channel_disabled": return "Turn the channel on first."
        case "channel_not_configured": return "Add the channel first."
        case "ai_unavailable": return "The marketing assistant is not available right now."
        case "rate_limited": return "Too many requests for now. Try again in a minute."
        case "campaign_not_found", "ad_not_found", "experiment_not_found", "keyword_not_found", "post_not_found",
             "reddit_thread_not_found", "funnel_week_not_found":
            return "It was removed or you no longer have access."
        case "zoho_code_incomplete": return "Enter the Self Client's client ID, its client secret and the code."
        case "zoho_code_rejected":
            return "Zoho rejected the code in every data center. Codes work once and expire in minutes: generate a new one."
        case "zoho_account_missing": return "Zoho accepted the code, but that login has no Zoho Mail account."
        case "offline": return "You are offline."
        case "desktop_unavailable": return "Your desktop is not connected."
        case "": return status > 0 ? "The server answered HTTP \(status)." : "The request failed."
        default: return code.replacingOccurrences(of: "_", with: " ").capitalizedFirst
        }
    }

    public static func from(_ reply: PackagesReply) -> PackagesError {
        var fields: [String: String] = [:]
        for (key, value) in reply.json.obj("fields") { fields[key] = value.text }
        let contact = reply.json.obj("contact")
        if PackagesText.isHexId(contact.str("id")) {
            fields["contactId"] = contact.str("id")
            fields["contactName"] = contact.str("name")
            fields["contactEmail"] = contact.str("email")
        }
        return PackagesError(reply.json.str("error"), status: reply.status, fields: fields)
    }

    public static func isTransient(_ error: Error) -> Bool {
        if let error = error as? PackagesError { return error.status == 408 || error.status == 425 || error.status >= 500 }
        if let error = error as? HTTPError { return error != .cancelled }
        return false
    }

    public static func describe(_ error: Error) -> String {
        if let error = error as? PackagesError { return error.message }
        if let error = error as? HTTPError {
            switch error {
            case .timedOut: return "The server took too long to answer."
            case .cancelled: return "Cancelled."
            default: return "Couldn't reach the server."
            }
        }
        return "Something went wrong."
    }
}

extension String {
    var capitalizedFirst: String {
        guard let first else { return self }
        return first.uppercased() + dropFirst()
    }
}

@MainActor
public protocol PackagesTransport: AnyObject {
    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws
        -> PackagesReply
}

@MainActor
public final class ApiPackagesTransport: PackagesTransport {
    private let api: ApiService

    public init(api: ApiService) {
        self.api = api
    }

    public func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws
        -> PackagesReply
    {
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if body != nil { headers["Content-Type"] = "application/json" }
        if method != "GET" { headers["Origin"] = api.gitOrigin }
        let response = try await api.rawRequest(
            method, api.url(path, query), headers: headers, body: body.map { JSON.encodeData(.object($0)) })
        return PackagesReply(status: response.status, json: await Self.parse(response.body))
    }

    nonisolated static let detachedParseBytes: Int = 16 * 1024

    nonisolated static func parse(_ body: Data) async -> JSONObject {
        if body.count < detachedParseBytes { return JSON.parseObject(body) }
        return await Task.detached(priority: .userInitiated) { JSON.parseObject(body) }.value
    }
}
