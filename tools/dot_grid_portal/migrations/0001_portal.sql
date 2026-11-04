PRAGMA foreign_keys = ON;
CREATE TABLE tenants (id TEXT PRIMARY KEY, name TEXT NOT NULL);
CREATE TABLE members (
 email TEXT PRIMARY KEY COLLATE NOCASE, tenant_id TEXT NOT NULL REFERENCES tenants(id),
 role TEXT NOT NULL CHECK(role IN ('admin','member')), active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),
 invited_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE designs (
 seed INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL, tenant_id TEXT REFERENCES tenants(id),
 name TEXT NOT NULL, extension TEXT NOT NULL, units TEXT NOT NULL, status TEXT NOT NULL,
 inspection TEXT, registry_entry TEXT, error TEXT, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
-- Bundled registry already owns seed 7. Reservation is permanent, never deleted.
INSERT INTO designs(seed,id,name,extension,units,status) VALUES(7,'reserved-bundled-seeds','Bundled seeds','','um','reserved');
CREATE INDEX tenant_designs ON designs(tenant_id, created_at);
CREATE TABLE feedback (id INTEGER PRIMARY KEY AUTOINCREMENT, tenant_id TEXT NOT NULL, email TEXT NOT NULL, text TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
