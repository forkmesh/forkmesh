package com.forkmesh.nativeapp.feature.account.projects

import android.content.Context
import com.forkmesh.nativeapp.account.projects.ProjectScope
import java.io.File

object AppFiles {
    fun documentsDir(context: Context): File = context.getDir("flutter", Context.MODE_PRIVATE)

    fun supportDir(context: Context): File = context.filesDir

    fun projectDocumentsDir(context: Context, scope: ProjectScope): File = scope.directory(documentsDir(context))

    fun projectSupportDir(context: Context, scope: ProjectScope): File = scope.directory(supportDir(context))
}
