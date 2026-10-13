package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.testing.assertEq

class LoginSubtitleTest {
    fun testNamesTheProjectOnlyWhenThereIsAChoice() {
        val home = Project("forkmesh", "ForkMesh", "https://app.forkmesh.com")
        val lab = Project("lab", "Home lab", "https://lab.example")
        assertEq("Continue with your ForkMesh account.", loginSubtitle(listOf(home), home))
        assertEq("Continue with your ForkMesh account.", loginSubtitle(listOf(home, lab), null))
        assertEq(
            "Signing in to Home lab (lab). Your accounts in your other projects are untouched.",
            loginSubtitle(listOf(home, lab), lab),
        )
    }
}
