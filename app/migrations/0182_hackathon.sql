-- ForkMesh Hackathon: campaign, participants, points, squads, share events,
-- milestones, mission progress, challenge links, the reward ledger, teams,
-- submissions, judging, community votes and the ambassador programme.
--
-- A campaign is a judged build contest wrapped in a referral, squad and
-- mission layer, so that taking part in it is itself the marketing. Nothing
-- here is visible until an administrator advances the campaign out of
-- 'draft': every public route answers 404 while it is a draft, which is what
-- makes it safe to land this on a repository where every push deploys.
--
-- Deliberately absent, and staying absent:
--
--   * No contact of any kind. Sharing happens in the participant's own
--     client through a link they can still edit; ForkMesh never collects an
--     address book and never messages a third party on a participant's
--     behalf, so there is no recipient column here to hold one.
--   * No referral decision. hackathon_participants.referrer_bi records who
--     brought a participant in, for squad placement and the growth funnel.
--     Whether that referral is *credited* stays entirely in
--     referral_attributions, behind the maturity, proved-device, burst and
--     network rules the public referral board already enforces. This feature
--     reads that decision and never re-implements it.
--   * No random element. Every reward is a fixed threshold, which is what
--     keeps the contest a skill contest rather than a sweepstakes.
--   * No membership row a billing provider owns. Prize months are written
--     to hackathon_reward_grants and projected into role_grants under a
--     promotion scope; Polar recomputes and Patreon revokes platform-scope
--     membership rows on every sync, and a prize must survive that.
--
-- Counters are denormalised onto hackathons and hackathon_participants
-- because D1 bills rows read and the landing page's milestone meter is the
-- hottest read in the feature; neither it nor a board may run COUNT(*).

CREATE TABLE IF NOT EXISTS hackathons (
        slug TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'draft'
            CHECK (status IN ('draft','registration','live','judging','closed')),
        config TEXT NOT NULL DEFAULT '{}',
        starts_at INTEGER NOT NULL DEFAULT 0,
        ends_at INTEGER NOT NULL DEFAULT 0,
        judging_ends_at INTEGER NOT NULL DEFAULT 0,
        participant_count INTEGER NOT NULL DEFAULT 0,
        verified_count INTEGER NOT NULL DEFAULT 0,
        squad_count INTEGER NOT NULL DEFAULT 0,
        submission_count INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS hackathon_participants (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        referrer_bi TEXT NOT NULL DEFAULT '',
        referrer_name TEXT NOT NULL DEFAULT '',
        squad_id TEXT NOT NULL DEFAULT '',
        track TEXT NOT NULL DEFAULT '',
        points INTEGER NOT NULL DEFAULT 0,
        onboarding_step INTEGER NOT NULL DEFAULT 1,
        share_count INTEGER NOT NULL DEFAULT 0,
        referrals_joined INTEGER NOT NULL DEFAULT 0,
        referrals_qualified INTEGER NOT NULL DEFAULT 0,
        joined_at INTEGER NOT NULL DEFAULT 0,
        verified_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi));

CREATE INDEX IF NOT EXISTS idx_hackathon_participants_points ON hackathon_participants(hackathon_slug, points DESC, joined_at);

CREATE INDEX IF NOT EXISTS idx_hackathon_participants_recruiters ON hackathon_participants(hackathon_slug, referrals_qualified DESC, points DESC);

CREATE INDEX IF NOT EXISTS idx_hackathon_participants_referrer ON hackathon_participants(hackathon_slug, referrer_bi);

CREATE INDEX IF NOT EXISTS idx_hackathon_participants_squad ON hackathon_participants(hackathon_slug, squad_id, points DESC);

CREATE INDEX IF NOT EXISTS idx_hackathon_participants_name ON hackathon_participants(hackathon_slug, name);

CREATE TABLE IF NOT EXISTS hackathon_points (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        kind TEXT NOT NULL,
        ref TEXT NOT NULL DEFAULT '',
        points INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, kind, ref));

CREATE INDEX IF NOT EXISTS idx_hackathon_points_account ON hackathon_points(hackathon_slug, account_bi, created_at DESC);

CREATE TABLE IF NOT EXISTS hackathon_squads (
        squad_id TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        code TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        member_count INTEGER NOT NULL DEFAULT 1,
        points INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0);

CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_squads_code ON hackathon_squads(hackathon_slug, code);

CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_squads_owner ON hackathon_squads(hackathon_slug, owner_bi);

CREATE INDEX IF NOT EXISTS idx_hackathon_squads_rank ON hackathon_squads(hackathon_slug, points DESC, member_count DESC);

CREATE TABLE IF NOT EXISTS hackathon_share_events (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        channel TEXT NOT NULL,
        surface TEXT NOT NULL DEFAULT '',
        minute_bucket INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, channel, minute_bucket));

CREATE INDEX IF NOT EXISTS idx_hackathon_share_events_time ON hackathon_share_events(hackathon_slug, created_at DESC);

CREATE TABLE IF NOT EXISTS hackathon_milestones (
        hackathon_slug TEXT NOT NULL,
        key TEXT NOT NULL,
        metric TEXT NOT NULL,
        threshold INTEGER NOT NULL DEFAULT 0,
        label TEXT NOT NULL DEFAULT '',
        description TEXT NOT NULL DEFAULT '',
        reward TEXT NOT NULL DEFAULT '{}',
        unlocked_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, key));

CREATE TABLE IF NOT EXISTS hackathon_mission_progress (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        mission_key TEXT NOT NULL,
        progress INTEGER NOT NULL DEFAULT 0,
        completed_at INTEGER NOT NULL DEFAULT 0,
        rewarded_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, mission_key));

CREATE INDEX IF NOT EXISTS idx_hackathon_mission_progress_account ON hackathon_mission_progress(hackathon_slug, account_bi);

CREATE TABLE IF NOT EXISTS hackathon_challenges (
        token TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        challenger_bi TEXT NOT NULL,
        challenger_name TEXT NOT NULL,
        target_points INTEGER NOT NULL DEFAULT 0,
        accepted_count INTEGER NOT NULL DEFAULT 0,
        beaten_count INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL DEFAULT 0);

CREATE INDEX IF NOT EXISTS idx_hackathon_challenges_challenger ON hackathon_challenges(hackathon_slug, challenger_bi, created_at DESC);

CREATE TABLE IF NOT EXISTS hackathon_challenge_entries (
        token TEXT NOT NULL,
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        accepted_at INTEGER NOT NULL DEFAULT 0,
        beaten_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (token, account_bi));

CREATE INDEX IF NOT EXISTS idx_hackathon_challenge_entries_account ON hackathon_challenge_entries(hackathon_slug, account_bi, beaten_at);

CREATE TABLE IF NOT EXISTS hackathon_reward_grants (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        reward_key TEXT NOT NULL,
        kind TEXT NOT NULL
            CHECK (kind IN ('pro_months','supporter_months','badge')),
        months INTEGER NOT NULL DEFAULT 0,
        badge TEXT NOT NULL DEFAULT '',
        granted_at INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, reward_key, kind));

CREATE INDEX IF NOT EXISTS idx_hackathon_reward_grants_account ON hackathon_reward_grants(hackathon_slug, account_bi, granted_at DESC);

CREATE TABLE IF NOT EXISTS hackathon_teams (
        team_id TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        name TEXT NOT NULL,
        captain_bi TEXT NOT NULL,
        captain_name TEXT NOT NULL,
        invite_code TEXT NOT NULL,
        member_count INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL DEFAULT 0);

CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_teams_code ON hackathon_teams(hackathon_slug, invite_code);

CREATE TABLE IF NOT EXISTS hackathon_team_members (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        team_id TEXT NOT NULL,
        name TEXT NOT NULL,
        joined_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi));

CREATE INDEX IF NOT EXISTS idx_hackathon_team_members_team ON hackathon_team_members(team_id);

CREATE TABLE IF NOT EXISTS hackathon_submissions (
        submission_id TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        team_id TEXT NOT NULL,
        track TEXT NOT NULL,
        repo_owner TEXT NOT NULL,
        repo_name TEXT NOT NULL,
        title TEXT NOT NULL,
        pitch TEXT NOT NULL DEFAULT '',
        demo_url TEXT NOT NULL DEFAULT '',
        video_url TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'needs_review'
            CHECK (status IN ('valid','needs_review','invalid','withdrawn')),
        review_flags TEXT NOT NULL DEFAULT '[]',
        placement TEXT NOT NULL DEFAULT ''
            CHECK (placement IN ('','first','second')),
        score_total INTEGER NOT NULL DEFAULT 0,
        score_count INTEGER NOT NULL DEFAULT 0,
        vote_count INTEGER NOT NULL DEFAULT 0,
        submitted_at INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL DEFAULT 0);

CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_submissions_team_track ON hackathon_submissions(hackathon_slug, team_id, track);

CREATE INDEX IF NOT EXISTS idx_hackathon_submissions_gallery ON hackathon_submissions(hackathon_slug, status, track, submitted_at);

CREATE TABLE IF NOT EXISTS hackathon_judges (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        added_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi));

CREATE TABLE IF NOT EXISTS hackathon_scores (
        submission_id TEXT NOT NULL,
        judge_bi TEXT NOT NULL,
        hackathon_slug TEXT NOT NULL,
        fit INTEGER NOT NULL CHECK (fit BETWEEN 1 AND 5),
        working INTEGER NOT NULL CHECK (working BETWEEN 1 AND 5),
        portability INTEGER NOT NULL CHECK (portability BETWEEN 1 AND 5),
        craft INTEGER NOT NULL CHECK (craft BETWEEN 1 AND 5),
        total INTEGER NOT NULL DEFAULT 0,
        scored_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (submission_id, judge_bi));

CREATE TABLE IF NOT EXISTS hackathon_votes (
        hackathon_slug TEXT NOT NULL,
        voter_bi TEXT NOT NULL,
        track TEXT NOT NULL,
        submission_id TEXT NOT NULL,
        voted_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, voter_bi, track));

CREATE INDEX IF NOT EXISTS idx_hackathon_votes_submission ON hackathon_votes(submission_id);

CREATE TABLE IF NOT EXISTS hackathon_ambassadors (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK (status IN ('pending','approved','rejected')),
        pitch TEXT NOT NULL DEFAULT '',
        applied_at INTEGER NOT NULL DEFAULT 0,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        reviewed_by TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (hackathon_slug, account_bi));

CREATE INDEX IF NOT EXISTS idx_hackathon_ambassadors_status ON hackathon_ambassadors(hackathon_slug, status, applied_at);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:6869abc284ab7fe669404086bd45c04513eb9696d42db5a2652270848ecad8e3',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
