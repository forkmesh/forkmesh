-- Locked Town Square placements gain a heading. The value is an offset in
-- radians from the object's authored rotation, so 0 (every pre-existing row,
-- and the column default) leaves the scene exactly as it was built.
ALTER TABLE world_object_layout ADD COLUMN rotation REAL NOT NULL DEFAULT 0;
