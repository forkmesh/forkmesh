-- Public profile presentation for cached remote fediverse actors.
--
-- A repo's follower list is public fediverse data (the remote server publishes
-- the same follow), but until now the cache kept only routing fields — inbox,
-- key, handle, display name. Rendering WHO follows a repository needs the two
-- fields every fediverse client already shows: the avatar and the bio.
--
-- Both are refreshed on the normal actor-document refresh (AP_REMOTE_ACTOR_TTL)
-- and on an Update(actor) broadcast, so nothing backfills here: existing rows
-- stay NULL until their next fetch.
ALTER TABLE ap_remote_actors ADD COLUMN avatar_url TEXT;
ALTER TABLE ap_remote_actors ADD COLUMN summary TEXT;
