package com.forkmesh.nativeapp.feature.code.collab

import com.forkmesh.nativeapp.ui.kit.confirmAction
import com.forkmesh.nativeapp.collab.PullMergeAllConfirmation
import com.forkmesh.nativeapp.collab.PullMergeAllRemote
import com.forkmesh.nativeapp.collab.PullMergeAllResult
import com.forkmesh.nativeapp.collab.PullMergeAllRules
import com.forkmesh.nativeapp.feature.code.review.CodeServices
import com.forkmesh.nativeapp.model.Repository
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal suspend fun CollabEnv.mergeAllPullsWithAi(repo: Repository) {
    val remote = runCatching { CodeServices.pullMergeAll() }.getOrNull()
    if (remote == null || !CodeServices.desktopPaired()) {
        status.error(PullMergeAllRules.UNPAIRED)
        return
    }
    val confirmation: PullMergeAllConfirmation = try {
        withContext(Dispatchers.IO) { remote.prepare(repo.owner, repo.name) }
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        status.error(PullMergeAllRemote.message(error))
        return
    }
    val confirmed = messenger.confirmAction(PullMergeAllRules.TITLE, PullMergeAllRules.TITLE, confirmation.summary)
    if (!confirmed) return
    status.show("Asking the desktop to start the merge agent…")
    val result: PullMergeAllResult = try {
        withContext(Dispatchers.IO) { remote.run(repo.owner, repo.name, confirmation.token) }
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        status.error(PullMergeAllRemote.message(error))
        return
    }
    status.show(result.message.ifEmpty { "The desktop started the merge agent." })
}
