package com.forkmesh.nativeapp.navigation

sealed class CodeButtonPlan {
    data object NoRepo : CodeButtonPlan()

    data object PopToRepo : CodeButtonPlan()

    data object Reset : CodeButtonPlan()
}

enum class CodeBarItem { Repos, Code }

object CodeButton {
    const val REPOS_LABEL = "Repos"
    const val REPOS_ICON = "library_books"
    const val CODE_LABEL = "Code"
    const val CODE_ICON = "code"
    const val NO_REPO_NOTICE = "Open a repository and Code brings you back to it"

    fun plan(current: RepoRef?, stack: List<RepoRef?>): CodeButtonPlan {
        val key = current?.slug?.lowercase() ?: return CodeButtonPlan.NoRepo
        return if (stack.firstOrNull()?.slug?.lowercase() == key) CodeButtonPlan.PopToRepo else CodeButtonPlan.Reset
    }

    fun selected(onCodeTab: Boolean, depth: Int): CodeBarItem? = when {
        !onCodeTab -> null
        depth > 1 -> CodeBarItem.Code
        else -> CodeBarItem.Repos
    }
}
