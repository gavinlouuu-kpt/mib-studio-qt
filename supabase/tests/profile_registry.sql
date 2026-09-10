-- Run after migration in a disposable database. Rolls back all fixtures.
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
 ('20000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000001',ARRAY['author','reviewer']),
 ('20000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000002',ARRAY['reviewer','publisher']),
 ('20000000-0000-0000-0000-000000000001','00000000-0000-0000-0000-000000000003',ARRAY['viewer']),
 ('20000000-0000-0000-0000-000000000002','00000000-0000-0000-0000-000000000004',ARRAY['admin']);
INSERT INTO public.registry_methods(method_id,project_id,display_name) VALUES
 ('30000000-0000-0000-0000-000000000001','20000000-0000-0000-0000-000000000001','Method A'),
 ('30000000-0000-0000-0000-000000000002','20000000-0000-0000-0000-000000000002','Method B');
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
CREATE FUNCTION pg_temp.submit_fixture(id uuid, parent text DEFAULT '', head text DEFAULT '') RETURNS jsonb
LANGUAGE sql AS $$
 SELECT public.registry_submit('30000000-0000-0000-0000-000000000001',id,parent,head,
 '{"camera_script":"","config":{"config_schema_version":1},"declared_hardware_compatibility":{},"method_schema_version":1,"processing_contract_version":1,"processing_core_id":"core"}',
 encode(sha256(convert_to('{"camera_script":"","config":{"config_schema_version":1},"declared_hardware_compatibility":{},"method_schema_version":1,"processing_contract_version":1,"processing_core_id":"core"}','UTF8')),'hex'));
$$;
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000001',true);
SET LOCAL ROLE authenticated;
DO $$ BEGIN
    IF (SELECT count(*) FROM public.registry_projects)<>1 THEN RAISE EXCEPTION 'project isolation failed'; END IF;
    IF (SELECT count(*) FROM public.registry_methods)<>1 THEN RAISE EXCEPTION 'method isolation failed'; END IF;
END $$;
SELECT pg_temp.expect_error('UPDATE public.registry_memberships SET roles=ARRAY[''admin'']','42501');
SELECT pg_temp.expect_error('INSERT INTO public.registry_methods(project_id,display_name) VALUES(''20000000-0000-0000-0000-000000000001'',''bypass'')','42501');
SELECT pg_temp.expect_error('DELETE FROM public.registry_methods','42501');
SELECT public.registry_submit('30000000-0000-0000-0000-000000000001','40000000-0000-0000-0000-000000000001','','',
 '{"camera_script":"","config":{"config_schema_version":1},"declared_hardware_compatibility":{},"method_schema_version":1,"processing_contract_version":1,"processing_core_id":"core"}',
 encode(sha256(convert_to('{"camera_script":"","config":{"config_schema_version":1},"declared_hardware_compatibility":{},"method_schema_version":1,"processing_contract_version":1,"processing_core_id":"core"}','UTF8')),'hex'));
SELECT pg_temp.submit_fixture('40000000-0000-0000-0000-000000000001');
DO $$ BEGIN
    IF (SELECT count(*) FROM public.registry_revisions)<>1 THEN RAISE EXCEPTION 'submit retry duplicated revision'; END IF;
END $$;
SELECT pg_temp.expect_error('SELECT public.registry_submit(''30000000-0000-0000-0000-000000000001'',''40000000-0000-0000-0000-000000000009'','''','''',''{}'',''bad'')','22023');
SELECT pg_temp.expect_error('UPDATE public.registry_revisions SET state=''published''','42501');
SELECT pg_temp.expect_error('DELETE FROM public.registry_revisions','42501');
SELECT pg_temp.expect_error('INSERT INTO public.registry_audit_events(project_id,action) VALUES(''20000000-0000-0000-0000-000000000001'',''forged'')','42501');
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000001'',''approved'',1,''self approve'')','42501');
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000003',true);
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000001'',''approved'',1,''viewer approve'')','42501');
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000002',true);
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000001'',''published'',1,''skip approval'')','PT409');
SELECT public.registry_transition('40000000-0000-0000-0000-000000000001','approved',1,'review evidence');
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000001'',''published'',1,''stale'')','PT409');
SELECT public.registry_transition('40000000-0000-0000-0000-000000000001','published',2,'pilot release');
SELECT public.registry_transition('40000000-0000-0000-0000-000000000001','revoked',3,'failed validation');
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000001'',''published'',4,''undo revoke'')','PT409');
DO $$ BEGIN
    IF (SELECT count(*) FROM public.registry_reviews)<>1 THEN RAISE EXCEPTION 'review audit missing'; END IF;
    IF (SELECT count(*) FROM public.registry_audit_events)<>4 THEN RAISE EXCEPTION 'transition audit missing'; END IF;
END $$;
-- Two candidates branch from the same published head. Publishing one cannot
-- silently rebase the other; failed publication leaves its approval intact.
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000001',true);
SELECT pg_temp.expect_error('SELECT pg_temp.submit_fixture(''40000000-0000-0000-0000-000000000002'')','PT409');
SELECT pg_temp.submit_fixture('40000000-0000-0000-0000-000000000002','40000000-0000-0000-0000-000000000001','40000000-0000-0000-0000-000000000001');
SELECT pg_temp.submit_fixture('40000000-0000-0000-0000-000000000003','40000000-0000-0000-0000-000000000001','40000000-0000-0000-0000-000000000001');
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000002',true);
SELECT public.registry_transition('40000000-0000-0000-0000-000000000002','approved',1,'approved');
SELECT public.registry_transition('40000000-0000-0000-0000-000000000003','approved',1,'approved');
SELECT public.registry_transition('40000000-0000-0000-0000-000000000002','published',2,'replacement');
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000003'',''published'',2,''stale parent'')','PT409');
DO $$ BEGIN
 IF (SELECT state FROM public.registry_revisions WHERE revision_id='40000000-0000-0000-0000-000000000003')<>'approved' THEN
   RAISE EXCEPTION 'failed publish mutated revision'; END IF;
END $$;
SELECT set_config('request.jwt.claim.sub','00000000-0000-0000-0000-000000000004',true);
DO $$ BEGIN
    IF (SELECT count(*) FROM public.registry_revisions)<>0 THEN RAISE EXCEPTION 'cross-project revision leak'; END IF;
    IF (SELECT count(*) FROM public.registry_reviews)<>0 THEN RAISE EXCEPTION 'cross-project review leak'; END IF;
    IF (SELECT count(*) FROM public.registry_audit_events)<>0 THEN RAISE EXCEPTION 'cross-project audit leak'; END IF;
END $$;
SELECT pg_temp.expect_error('SELECT public.registry_fetch_revision(''40000000-0000-0000-0000-000000000001'')','PT404');
SELECT pg_temp.expect_error('SELECT public.registry_transition(''40000000-0000-0000-0000-000000000001'',''archived'',4,''cross tenant'')','42501');
RESET ROLE;
SET LOCAL ROLE anon;
SELECT pg_temp.expect_error('SELECT * FROM public.registry_revisions','42501');
SELECT pg_temp.expect_error('SELECT public.registry_fetch_revision(''40000000-0000-0000-0000-000000000001'')','42501');
RESET ROLE;
ROLLBACK;
