// Lightweight PostgreSQL engine smoke test. Hosted Supabase/PostgREST/Auth
// deployment tests remain a separate gate; this harness stubs only auth.uid().
import { PGlite } from '@electric-sql/pglite';
import { readFile, readdir } from 'node:fs/promises';
const db = new PGlite();
try {
  await db.exec(`CREATE ROLE anon; CREATE ROLE authenticated;
    CREATE SCHEMA auth; CREATE TABLE auth.users(id uuid PRIMARY KEY);
    CREATE FUNCTION auth.uid() RETURNS uuid LANGUAGE sql STABLE AS
      $$ SELECT NULLIF(current_setting('request.jwt.claim.sub',true),'')::uuid $$;
    GRANT USAGE ON SCHEMA auth TO authenticated, anon;
    GRANT EXECUTE ON FUNCTION auth.uid() TO authenticated, anon;`);
  // Apply every migration in filename (timestamp) order, as Supabase does.
  const migrations = (await readdir(new URL('./migrations/', import.meta.url))).filter((f) => f.endsWith('.sql')).sort();
  for (const file of migrations)
    await db.exec(await readFile(new URL(`./migrations/${file}`, import.meta.url), 'utf8'));
  await db.exec(await readFile(new URL('./tests/profile_registry.sql', import.meta.url), 'utf8'));
  console.log('Registry PostgreSQL migration, RLS and lifecycle tests passed');
} finally {
  await db.close();
}
