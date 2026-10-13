-- Per-repository operational-alert switches (adhoc #445): the org admin of a
-- repository decides whether ForkMesh mails its administrators when a system
-- on /status goes down and when it recovers. Before this table the
-- "[ForkMesh outage]" / "[ForkMesh recovered]" status and cron-watchdog mail
-- was unconditional, so every admin account inherited the alert pager.
--
-- repo_bi is a blind index of "repo-alert-settings:<owner>/<repo>" (both
-- halves lowercased, org aliases resolved to the backing node like
-- ap_repo_settings). data is plaintext JSON of booleans — operational config,
-- not user content, so the send path never decrypts. A missing row means all
-- alert mail is OFF: nothing is sent until an admin turns it on.
CREATE TABLE IF NOT EXISTS repo_alert_settings (
    repo_bi TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    updated_at INTEGER NOT NULL);
