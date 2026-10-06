-- #398 M3 authoring RPCs. Run after every migration in a disposable database;
-- rolls back all fixtures.
BEGIN;
INSERT INTO auth.users(id) VALUES
 ('00000000-0000-0000-0000-000000000001'),
 ('00000000-0000-0000-0000-000000000002'),
 ('00000000-0000-0000-0000-000000000003'),
 ('00000000-0000-0000-0000-000000000004');
INSERT INTO public.registry_organizations VALUES('10000000-0000-0000-0000-000000000001','Org');
INSERT INTO public.registry_projects VALUES
 ('20000000-0000-0000-0000-000000000001','10000000-0000-0000-0000-000000000001','Project A'),
 ('20000000-0000-0000-0000-000000000002','10000000-0000-0000-0000-000000000001','Project B');
INSERT INTO public.registry_memberships VALUES
 ('20000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000001',ARRAY['author']),
 ('20000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000002',ARRAY['reviewer','publisher']),
 ('20000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000003',ARRAY['viewer']),
 ('20000000-0000-0000-0000-000000000002','00000000-0000-0000-0000-000000000004',ARRAY['author']);
CREATE FUNCTION pg_temp.expect_error(command text, expected text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    BEGIN EXECUTE command;
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE=expected THEN RETURN; END IF;
        RAISE EXCEPTION 'Expected %, got %: %',expected,SQLSTATE,SQLERRM;
    END;
    RAISE EXCEPTION 'Expected % but command succeeded',expected;
END;
$$;
CREATE FUNCTION pg_temp.content() RETURNS text LANGUAGE sql AS $$
 SELECT '{"camera_script":"","config":{"config_schema_version":1},"declared_hardware_compatibility":{},"method_schema_version":1,"processing_contract_version":1,"processing_core_id":"core"}'::text $$;
CREATE FUNCTION pg_temp.submit(id uuid, notes text, parent text DEFAULT '', head text DEFAULT '') RETURNS jsonb
LANGUAGE sql AS $$
 SELECT public.registry_submit('30000000-0000-0000-0000-00000000000a',id,parent,head,pg_temp.content(),
   encode(sha256(convert_to(pg_temp.content(),'UTF8')),'hex'),notes) $$;

SET LOCAL ROLE authenticated;

-- A viewer cannot create methods; an author can, idempotently.
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000003',true);
SELECT pg_temp.expect_error($q$SELECT public.registry_create_method('20000000-0000-0000-0000-000000000001','30000000-0000-0000-0000-00000000000a','Sorting','')$q$,'42501');
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000001',true);
SELECT pg_temp.expect_error($q$SELECT public.registry_create_method('20000000-0000-0000-0000-000000000001','30000000-0000-0000-0000-00000000000a','   ','')$q$,'22023');
DO $$ DECLARE created jsonb; BEGIN
    created := public.registry_create_method('20000000-0000-0000-0000-000000000001',
        '30000000-0000-0000-0000-00000000000a','  Sorting  ','Cell sorting method');
    IF created->>'display_name'<>'Sorting' OR created->'head_revision_id'<>'null'::jsonb THEN
        RAISE EXCEPTION 'create_method result: %', created; END IF;
    IF public.registry_create_method('20000000-0000-0000-0000-000000000001',
        '30000000-0000-0000-0000-00000000000a','Sorting','Cell sorting method')->>'method_id'
        <>'30000000-0000-0000-0000-00000000000a' THEN RAISE EXCEPTION 'create_method retry not idempotent'; END IF;
END $$;
SELECT pg_temp.expect_error($q$SELECT public.registry_create_method('20000000-0000-0000-0000-000000000001','30000000-0000-0000-0000-00000000000a','Other','')$q$,'PT409');
DO $$ DECLARE methods jsonb := public.registry_list_methods('20000000-0000-0000-0000-000000000001')->'methods'; BEGIN
    IF jsonb_array_length(methods)<>1 OR methods->0->>'method_id'<>'30000000-0000-0000-0000-00000000000a'
       OR methods->0->'head_revision_id'<>'null'::jsonb OR methods->0->>'description'<>'Cell sorting method' THEN
        RAISE EXCEPTION 'list_methods: %', methods; END IF;
    IF (SELECT count(*) FROM public.registry_audit_events WHERE action='method_created')<>1 THEN
        RAISE EXCEPTION 'method creation not audited once'; END IF;
END $$;

-- Release notes travel with the immutable revision; a retry must match them.
DO $$ BEGIN
    IF pg_temp.submit('40000000-0000-0000-0000-00000000000a','First release')->>'release_notes'<>'First release' THEN
        RAISE EXCEPTION 'release notes not returned'; END IF;
    PERFORM pg_temp.submit('40000000-0000-0000-0000-00000000000a','First release');
    IF (SELECT count(*) FROM public.registry_revisions)<>1 THEN RAISE EXCEPTION 'retry duplicated'; END IF;
END $$;
SELECT pg_temp.expect_error($q$SELECT pg_temp.submit('40000000-0000-0000-0000-00000000000a','Edited notes')$q$,'PT409');
SELECT pg_temp.expect_error($q$SELECT pg_temp.submit('40000000-0000-0000-0000-00000000000b',repeat('x',16385))$q$,'22023');
-- Older six-argument callers still work (empty notes).
SELECT public.registry_submit('30000000-0000-0000-0000-00000000000a','40000000-0000-0000-0000-00000000000c','','',
  pg_temp.content(),encode(sha256(convert_to(pg_temp.content(),'UTF8')),'hex'));

-- Publish: the head moves and the history lists reviews + audit events.
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000002',true);
SELECT public.registry_transition('40000000-0000-0000-0000-00000000000a','approved',1,'looks right');
SELECT public.registry_transition('40000000-0000-0000-0000-00000000000a','published',2,'pilot');
DO $$ DECLARE methods jsonb := public.registry_list_methods('20000000-0000-0000-0000-000000000001')->'methods';
              history jsonb := public.registry_revision_history('40000000-0000-0000-0000-00000000000a'); BEGIN
    IF methods->0->>'head_revision_id'<>'40000000-0000-0000-0000-00000000000a' THEN
        RAISE EXCEPTION 'head not exposed after publish: %', methods; END IF;
    IF jsonb_array_length(history->'reviews')<>1 OR history->'reviews'->0->>'decision'<>'approved'
       OR history->'reviews'->0->>'reason'<>'looks right' THEN
        RAISE EXCEPTION 'review history: %', history; END IF;
    IF (SELECT array_agg(e->>'action' ORDER BY ord) FROM jsonb_array_elements(history->'events') WITH ORDINALITY AS t(e,ord))
        <>ARRAY['submitted','approved','published'] THEN
        RAISE EXCEPTION 'audit history: %', history; END IF;
END $$;

-- Another project's author sees nothing and cannot create methods here.
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000004',true);
DO $$ BEGIN
    IF jsonb_array_length(public.registry_list_methods('20000000-0000-0000-0000-000000000001')->'methods')<>0 THEN
        RAISE EXCEPTION 'list_methods cross-project leak'; END IF;
END $$;
SELECT pg_temp.expect_error($q$SELECT public.registry_revision_history('40000000-0000-0000-0000-00000000000a')$q$,'PT404');
SELECT pg_temp.expect_error($q$SELECT public.registry_create_method('20000000-0000-0000-0000-000000000001','30000000-0000-0000-0000-00000000000b','Intruder','')$q$,'42501');
RESET ROLE;

-- Release notes are immutable even for the table owner.
SELECT pg_temp.expect_error($q$UPDATE public.registry_revisions SET release_notes='rewritten' WHERE revision_id='40000000-0000-0000-0000-00000000000a'$q$,'P0001');

SET LOCAL ROLE anon;
SELECT pg_temp.expect_error($q$SELECT public.registry_list_methods('20000000-0000-0000-0000-000000000001')$q$,'42501');
SELECT pg_temp.expect_error($q$SELECT public.registry_create_method('20000000-0000-0000-0000-000000000001','30000000-0000-0000-0000-00000000000c','x','')$q$,'42501');
RESET ROLE;
ROLLBACK;
