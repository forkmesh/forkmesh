package com.forkmesh.nativeapp.feature.push

import android.app.job.JobParameters
import android.app.job.JobService

class BackgroundLinkJob : JobService() {
    override fun onStartJob(params: JobParameters): Boolean {
        Thread({
            try {
                runOnce()
            } finally {
                jobFinished(params, false)
            }
        }, "BackgroundLinkJob").apply { isDaemon = true }.start()
        return true
    }

    override fun onStopJob(params: JobParameters): Boolean = true

    private fun runOnce() {
        val context = applicationContext
        val accounts = BackgroundLink.accounts()
        val plan = BackgroundLink.plan(context, accounts)
        if (!plan.runService) return
        val running = BackgroundLinkService.instance
        if (running != null) {
            running.apply()
            return
        }
        if (BackgroundLink.start(context)) return
        val many = accounts.size > 1
        val wanted = plan.projectIds.toSet()
        for (account in accounts) {
            if (account.id !in wanted) continue
            PingAlerts.fetchAndDeliver(context, account.id, account.label, many, account.api(), account.node)
        }
    }
}
