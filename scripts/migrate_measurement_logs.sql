-- Migrates measurement_logs from the pre-PR#1 layout to the current schema.
--
-- Old layout stored:  raw_reading = bust, scale_factor = waist,
--                     computed_dimension = hip, matched_sku = "<sku> [<silhouette>]"
-- New layout:         bust, waist, hip, matched_sku, silhouette, created_at
--
-- Usage (server stopped, backup taken first):
--   sqlite3 measureverse.db < scripts/migrate_measurement_logs.sql
-- Runs in one transaction: on any error nothing is changed.

PRAGMA foreign_keys = OFF;
BEGIN TRANSACTION;

ALTER TABLE measurement_logs RENAME TO measurement_logs_old;

CREATE TABLE measurement_logs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    bust        REAL NOT NULL,
    waist       REAL NOT NULL,
    hip         REAL NOT NULL,
    matched_sku TEXT NOT NULL,
    silhouette  TEXT NOT NULL,
    created_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

-- Split "<sku> [<silhouette>]" back into two columns. Rows without the
-- bracketed suffix keep the full text as the SKU and get silhouette 'Unknown'.
INSERT INTO measurement_logs (bust, waist, hip, matched_sku, silhouette)
SELECT
    COALESCE(raw_reading, 0),
    COALESCE(scale_factor, 0),
    COALESCE(computed_dimension, 0),
    CASE WHEN instr(matched_sku, ' [') > 0 AND substr(matched_sku, -1) = ']'
         THEN substr(matched_sku, 1, instr(matched_sku, ' [') - 1)
         ELSE COALESCE(matched_sku, '') END,
    CASE WHEN instr(matched_sku, ' [') > 0 AND substr(matched_sku, -1) = ']'
         THEN substr(matched_sku, instr(matched_sku, ' [') + 2,
                     length(matched_sku) - instr(matched_sku, ' [') - 2)
         ELSE 'Unknown' END
FROM measurement_logs_old
ORDER BY rowid;

DROP TABLE measurement_logs_old;

COMMIT;
PRAGMA foreign_keys = ON;
