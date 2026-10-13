-- Reverse 0182_hackathon.sql.
--
-- Safe because the hackathon tables hold only campaign bookkeeping that is
-- derived from, or re-derivable against, records that live elsewhere:
-- accounts, the referral attribution ledger, the catalog and the repository's
-- own signed collaboration files. Nothing here is the sole copy of anything.
-- No referral decision or badge award is touched: those live in
-- referral_attributions and badge_awards and survive this rollback.
-- Promotion-scope membership grants already written to role_grants also
-- survive and expire on their own dates; the rollback removes the ledger
-- that explains them, so export hackathon_reward_grants first if the
-- reasons must be kept.
DROP INDEX IF EXISTS idx_hackathon_ambassadors_status;
DROP TABLE IF EXISTS hackathon_ambassadors;
DROP INDEX IF EXISTS idx_hackathon_votes_submission;
DROP TABLE IF EXISTS hackathon_votes;
DROP TABLE IF EXISTS hackathon_scores;
DROP TABLE IF EXISTS hackathon_judges;
DROP INDEX IF EXISTS idx_hackathon_submissions_gallery;
DROP INDEX IF EXISTS idx_hackathon_submissions_team_track;
DROP TABLE IF EXISTS hackathon_submissions;
DROP INDEX IF EXISTS idx_hackathon_team_members_team;
DROP TABLE IF EXISTS hackathon_team_members;
DROP INDEX IF EXISTS idx_hackathon_teams_code;
DROP TABLE IF EXISTS hackathon_teams;
DROP INDEX IF EXISTS idx_hackathon_reward_grants_account;
DROP TABLE IF EXISTS hackathon_reward_grants;
DROP INDEX IF EXISTS idx_hackathon_challenge_entries_account;
DROP TABLE IF EXISTS hackathon_challenge_entries;
DROP INDEX IF EXISTS idx_hackathon_challenges_challenger;
DROP TABLE IF EXISTS hackathon_challenges;
DROP INDEX IF EXISTS idx_hackathon_mission_progress_account;
DROP TABLE IF EXISTS hackathon_mission_progress;
DROP TABLE IF EXISTS hackathon_milestones;
DROP INDEX IF EXISTS idx_hackathon_share_events_time;
DROP TABLE IF EXISTS hackathon_share_events;
DROP INDEX IF EXISTS idx_hackathon_squads_rank;
DROP INDEX IF EXISTS idx_hackathon_squads_owner;
DROP INDEX IF EXISTS idx_hackathon_squads_code;
DROP TABLE IF EXISTS hackathon_squads;
DROP INDEX IF EXISTS idx_hackathon_points_account;
DROP TABLE IF EXISTS hackathon_points;
DROP INDEX IF EXISTS idx_hackathon_participants_name;
DROP INDEX IF EXISTS idx_hackathon_participants_squad;
DROP INDEX IF EXISTS idx_hackathon_participants_referrer;
DROP INDEX IF EXISTS idx_hackathon_participants_recruiters;
DROP INDEX IF EXISTS idx_hackathon_participants_points;
DROP TABLE IF EXISTS hackathon_participants;
DROP TABLE IF EXISTS hackathons;
