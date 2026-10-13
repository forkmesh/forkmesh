package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonMaps

val ORG_ROLES: List<String> = listOf("owner", "admin", "maintainer", "member", "auditor", "guest")

val ORG_TEAM_PERMISSIONS: List<String> = listOf("read", "write", "maintain", "admin")

data class OrgSummary(
    val name: String,
    val role: String = "member",
) {
    val canManage: Boolean get() = role == "owner" || role == "admin"

    companion object {
        fun fromJson(json: Map<String, Any?>): OrgSummary = OrgSummary(
            name = firstModelString(json, listOf("name", "org")),
            role = modelString(json["role"]).trim().lowercase(),
        )
    }
}

data class OrgProfile(
    val name: String,
    val displayName: String = "",
    val description: String = "",
    val createdMs: Long = 0,
    val members: Int = 0,
    val teams: Int = 0,
    val repos: List<OrgRepo> = emptyList(),
    val viewerRole: String = "",
) {
    val title: String get() = if (displayName.isNotEmpty()) displayName else name

    val canManage: Boolean get() = viewerRole == "owner" || viewerRole == "admin"

    val isOwner: Boolean get() = viewerRole == "owner"

    companion object {
        fun fromJson(json: Map<String, Any?>): OrgProfile = OrgProfile(
            name = firstModelString(json, listOf("org", "name")),
            displayName = modelString(json["displayName"]),
            description = modelString(json["description"]),
            createdMs = modelInt(json["createdAt"]),
            members = modelInt(json["members"]).toInt(),
            teams = modelInt(json["teams"]).toInt(),
            repos = jsonMaps(json["repos"]).map { OrgRepo.fromJson(LinkedHashMap(it)) }.filter { it.repo.isNotEmpty() },
            viewerRole = modelString(json["viewerRole"]).trim().lowercase(),
        )
    }
}

data class OrgMember(
    val name: String,
    val role: String = "member",
    val sinceMs: Long = 0,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): OrgMember = OrgMember(
            name = modelString(json["name"]).trim().lowercase(),
            role = modelString(json["role"]).trim().lowercase(),
            sinceMs = modelInt(json["since"]),
        )
    }
}

data class OrgTeam(
    val team: String,
    val permission: String = "read",
    val members: Int = 0,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): OrgTeam = OrgTeam(
            team = modelString(json["team"]).trim().lowercase(),
            permission = modelString(json["permission"]).trim().lowercase(),
            members = modelInt(json["members"]).toInt(),
        )
    }
}

data class OrgRepo(
    val repo: String,
    val node: String = "",
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): OrgRepo = OrgRepo(
            repo = modelString(json["repo"]).trim().lowercase(),
            node = firstModelString(json, listOf("node", "node_owner")),
        )
    }
}

class OrgApiException(val code: String, override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

fun orgErrorMessage(code: String): String = when (code) {
    "invalid_org_name" -> "Invalid name. Use lowercase letters, numbers and dashes."
    "invalid_team_name" -> "Invalid team name. Use lowercase letters, numbers and dashes."
    "org_name_taken" -> "That name is already taken."
    "too_many_orgs" -> "You have reached the organization limit for this account."
    "too_many_members" -> "This organization has reached its member limit."
    "too_many_teams" -> "This organization has reached its team limit."
    "too_many_repos" -> "This organization has reached its linked-repo limit."
    "invalid_session" -> "Sign in to manage organizations."
    "forbidden" -> "You do not have permission to do that."
    "not_found" -> "Not found."
    "last_owner" -> "An organization must keep at least one owner."
    "unknown_account" -> "No account exists with that name."
    "not_a_member" -> "That account is not a member of this organization."
    "bad_role" -> "Invalid role."
    "bad_permission" -> "Invalid permission."
    "member_required" -> "Enter a member account name."
    "repo_required" -> "Enter a repository name."
    "not_your_node" -> "You can only link repositories from your own node."
    "unknown_repo" -> "That repository is not published on your node."
    "" -> "Request failed."
    else -> "Request failed ($code)."
}
