// Lightweight PostgreSQL engine smoke test. Hosted Supabase/PostgREST/Auth
// deployment tests remain a separate gate; this harness stubs only auth.uid().
import { PGlite } from '@electric-sql/pglite';
import { readFile } from 'node:fs/promises';
const db = new PGlite();
try {
  await db.exec(`CREATE ROLE anon; CREATE ROLE authenticated;
    CREATE SCHEMA auth; CREATE TABLE auth.users(id uuid PRIMARY KEY);
    CREATE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS
      $$ SELECT NULLIF(current_setting('request.jwt.claim.sub',true),'')::uuid $$;
    GRANT USAGE ON SCHEMA auth TO authenticated, anon;
    GRANT EXECUTE ON FUNCTION auth.uid() TO authenticated, anon;`);
  await db.exec(await readFile(new URL('./migrations/202609100001_profile_registry.sql', import.meta.url), 'utf8'));
  await db.exec(await readFile(new URL('./tests/profile_registry.sql', import.meta.url), 'utf8'));
  console.log('Registry PostgreSQL migration, RLS and lifecycle tests passed');
} finally {
  await db.close();
}
