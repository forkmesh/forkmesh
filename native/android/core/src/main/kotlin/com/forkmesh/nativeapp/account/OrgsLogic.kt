package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.ORG_ROLES
import com.forkmesh.nativeapp.model.ORG_TEAM_PERMISSIONS
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.model.OrgMember
import com.forkmesh.nativeapp.model.OrgProfile
import com.forkmesh.nativeapp.model.OrgRepo
import com.forkmesh.nativeapp.model.OrgSummary
import com.forkmesh.nativeapp.model.OrgTeam
import com.forkmesh.nativeapp.model.auth.AuthSession

fun orgErrorText(error: Throwable?): String =
    if (error is OrgApiException) error.message else "Something went wrong. Check your connection and try again."

enum class OrgRoleTone { Accent, Warning, Tertiary }

fun orgRoleTone(role: String): OrgRoleTone = when (role) {
    "owner" -> OrgRoleTone.Accent
    "admin" -> OrgRoleTone.Warning
    else -> OrgRoleTone.Tertiary
}

fun orgRoleLabel(role: String): String = role.ifEmpty { "member" }

fun canUseOrgs(session: AuthSession?): Boolean = hasRealSession(session)

fun orgsListStateKey(loading: Boolean, failed: Boolean, orgs: List<OrgSummary>): String = when {
    loading -> "loading"
    failed -> "error"
    orgs.isEmpty() -> "empty"
    else -> "list-${orgs.size}"
}

sealed class NewOrgName {
    data class Valid(val name: String) : NewOrgName()

    data class Invalid(val message: String) : NewOrgName()
}

fun newOrgName(raw: String): NewOrgName {
    val name = raw.trim().lowercase()
    return if (name.isEmpty()) NewOrgName.Invalid("Enter an organization name.") else NewOrgName.Valid(name)
}

data class OrgMemberInput(val name: String, val role: String)

fun orgMemberInput(name: String, role: String): OrgMemberInput? {
    val clean = name.trim().lowercase()
    return if (clean.isEmpty()) null else OrgMemberInput(clean, role)
}

data class OrgTeamInput(val name: String, val permission: String)

fun orgTeamInput(name: String, permission: String): OrgTeamInput? {
    val clean = name.trim().lowercase()
    return if (clean.isEmpty()) null else OrgTeamInput(clean, permission)
}

data class OrgRepoInput(val repo: String, val node: String)

fun orgRepoInput(repo: String, node: String): OrgRepoInput? {
    val clean = repo.trim().lowercase()
    return if (clean.isEmpty()) null else OrgRepoInput(clean, node.trim())
}

fun defaultRepoNode(session: AuthSession?): String = session?.nodeName?.trim()?.lowercase().orEmpty()

data class OrgMenuItem(val value: String, val label: String, val enabled: Boolean = true)

fun memberMenuRoles(member: OrgMember): List<OrgMenuItem> =
    ORG_ROLES.map { role -> OrgMenuItem(role, "Set role: $role", enabled = role != member.role) }

val MEMBER_MENU_REMOVE = OrgMenuItem("remove", "Remove from org")

fun teamMenuPermissions(team: OrgTeam): List<OrgMenuItem> =
    ORG_TEAM_PERMISSIONS.map { p -> OrgMenuItem("perm:$p", "Permission: $p", enabled = p != team.permission) }

val TEAM_MENU_DELETE = OrgMenuItem("delete", "Delete team")

sealed class OrgMenuAction {
    data object RemoveMember : OrgMenuAction()

    data class SetRole(val role: String) : OrgMenuAction()

    data object DeleteTeam : OrgMenuAction()

    data class SetPermission(val permission: String) : OrgMenuAction()
}

fun memberMenuAction(value: String): OrgMenuAction =
    if (value == "remove") OrgMenuAction.RemoveMember else OrgMenuAction.SetRole(value)

fun teamMenuAction(value: String): OrgMenuAction? = when {
    value == "delete" -> OrgMenuAction.DeleteTeam
    value.startsWith("perm:") -> OrgMenuAction.SetPermission(value.substring(5))
    else -> null
}

fun teamAddCandidates(orgMembers: List<OrgMember>, teamMembers: List<OrgMember>): List<OrgMember> {
    val onTeam = teamMembers.map { it.name }.toSet()
    return orgMembers.filter { it.name !in onTeam }
}

const val TEAM_ALREADY_FULL = "Every org member is already on this team."

fun teamMemberCountLabel(members: Int): String = "$members member${if (members == 1) "" else "s"}"

fun teamSubtitle(team: OrgTeam): String = "${team.permission} permission"

fun teamEmptyMessage(error: String, team: OrgTeam): String =
    error.ifEmpty { "Add org members to grant them the ${team.permission} permission over linked repos." }

fun linkedRepoTitle(orgName: String, repo: OrgRepo): String = "$orgName/${repo.repo}"

fun linkedRepoServes(repo: OrgRepo): String? = if (repo.node.isEmpty()) null else "serves ${repo.node}/${repo.repo}"

fun orgStats(profile: OrgProfile): List<Pair<String, Int>> =
    listOf("Members" to profile.members, "Teams" to profile.teams, "Repos" to profile.repos.size)

fun orgDeleteTitle(orgName: String): String = "Delete $orgName?"
