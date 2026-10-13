-- Preserve the exact normalized operation set covered by every mirror's
-- signed repository-health challenge. Reward selection recomputes its digest
-- and required-operation subset from this value; mutable eligibility booleans
-- are never sufficient.
ALTER TABLE mirror_https_endpoints
  ADD COLUMN forkmesh_operations_json TEXT NOT NULL DEFAULT '[]';
