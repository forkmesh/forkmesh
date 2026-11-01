-- Reverse 0199: drop the hosted-repository tables. The bare repositories on
-- the core nodes are not touched (see docs/operations/core-hosted-repositories.md).
DROP TABLE IF EXISTS hosted_repo_copies;
DROP TABLE IF EXISTS hosted_repos;
