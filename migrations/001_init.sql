CREATE TABLE IF NOT EXISTS feedback (
    id                TEXT PRIMARY KEY NOT NULL,
    content           TEXT NOT NULL,
    category          TEXT NULL,
    status            TEXT NOT NULL DEFAULT 'new',
    admin_note        TEXT NULL,
    created_at_bucket TEXT NOT NULL,
    updated_at        TEXT NULL,
    CONSTRAINT feedback_category_allowed
        CHECK (category IS NULL OR category IN ('bug', 'suggestion', 'complaint', 'other')),
    CONSTRAINT feedback_status_allowed
        CHECK (status IN ('new', 'read', 'archived')),
    CONSTRAINT feedback_content_len
        CHECK (length(content) BETWEEN 1 AND 5000),
    CONSTRAINT feedback_note_len
        CHECK (admin_note IS NULL OR length(admin_note) <= 2000)
);

CREATE INDEX IF NOT EXISTS idx_feedback_status_created
    ON feedback (status, created_at_bucket DESC);
