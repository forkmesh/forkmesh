CREATE TABLE IF NOT EXISTS bug_report_attachments (
    report_id TEXT NOT NULL CHECK (length(report_id) = 32),
    attachment_id TEXT NOT NULL CHECK (length(attachment_id) = 32),
    chunk_index INTEGER NOT NULL CHECK (chunk_index >= 0),
    name TEXT NOT NULL,
    content_type TEXT NOT NULL DEFAULT '',
    size INTEGER NOT NULL CHECK (size > 0),
    data TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at > 0),
    PRIMARY KEY (report_id, attachment_id, chunk_index)
);

CREATE INDEX IF NOT EXISTS idx_bug_report_attachments_report
    ON bug_report_attachments(report_id, attachment_id, chunk_index);
