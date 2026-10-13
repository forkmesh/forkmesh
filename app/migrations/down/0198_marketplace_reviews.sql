-- Reverse 0198: drop the account-based Market ratings and reviews.
DROP TABLE IF EXISTS marketplace_package_uses;
DROP TABLE IF EXISTS marketplace_package_rating_stats;
DROP TABLE IF EXISTS marketplace_review_reports;
DROP TABLE IF EXISTS marketplace_review_votes;
DROP TABLE IF EXISTS marketplace_review_history;
DROP TABLE IF EXISTS marketplace_reviews;
