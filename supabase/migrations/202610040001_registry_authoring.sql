-- Issue #398 M3: authoring support for the desktop registry worker.
--  * authors create methods (registry_create_method), idempotent by the
--    caller-generated method ID, audited;
--  * immutable release notes travel with each submitted revision;
--  * registry_list_methods exposes each method's published head so a client
--    can detect "draft based on r12, head is now r13" before submitting;
--  * registry_revision_history returns the reviews and audit events of a
--    revision (RLS applies: SECURITY INVOKER).
BEGIN;

ALTER TABLE public.registry_revisions
    ADD COLUMN release_notes text NOT NULL DEFAULT '' CHECK (octet_length(release_notes) <= 16384);

-- Release notes are part of the immutable revision record.
CREATE OR REPLACE FUNCTION registry_private.immutable_revision() RETURNS trigger LANGUAGE plpgsql SET search_path='' AS $$
BEGIN
    IF TG_OP='DELETE' THEN RAISE EXCEPTION 'Historical revisions cannot be deleted'; END IF;
    IF (NEW.revision_id,NEW.method_id,NEW.parent_revision_id,NEW.revision_number,NEW.author_id,
        NEW.canonical_content,NEW.content_hash,NEW.created_at,NEW.release_notes)
        IS DISTINCT FROM
       (OLD.revision_id,OLD.method_id,OLD.parent_revision_id,OLD.revision_number,OLD.author_id,
        OLD.canonical_content,OLD.content_hash,OLD.created_at,OLD.release_notes)
    THEN RAISE EXCEPTION 'Revision content and identity are immutable'; END IF;
    IF OLD.state='revoked' AND NEW.state<>'revoked' THEN RAISE EXCEPTION 'Revocation is terminal'; END IF;
    RETURN NEW;
END;
$$;

CREATE FUNCTION public.registry_create_method(p_project_id uuid,p_method_id uuid,p_display_name text,
    p_description text DEFAULT '') RETURNS jsonb
LANGUAGE plpgsql SECURITY DEFINER SET search_path='' AS $$
DECLARE existing public.registry_methods; name text := trim(coalesce(p_display_name,''));
BEGIN
    IF NOT registry_private.has_role(p_project_id,ARRAY['author']) THEN
        RAISE SQLSTATE '42501' USING MESSAGE='Author role required'; END IF;
    IF length(name)=0 OR length(name)>200 OR octet_length(coalesce(p_description,''))>16384 THEN
        RAISE SQLSTATE '22023' USING MESSAGE='Method name required (at most 200 characters)'; END IF;
    SELECT * INTO existing FROM public.registry_methods WHERE method_id=p_method_id;
    IF FOUND THEN
        -- Idempotent retry of the same creation only.
        IF existing.project_id=p_project_id AND existing.display_name=name AND
           existing.description=coalesce(p_description,'') THEN
            RETURN jsonb_build_object('method_id',existing.method_id,'project_id',existing.project_id,
                'display_name',existing.display_name,'description',existing.description,
                'head_revision_id',existing.head_revision_id);
        END IF;
        RAISE SQLSTATE 'PT409' USING MESSAGE='Method ID already used';
    END IF;
    INSERT INTO public.registry_methods(method_id,project_id,display_name,description)
    VALUES(p_method_id,p_project_id,name,coalesce(p_description,''));
    INSERT INTO public.registry_audit_events(project_id,actor_id,action,reason)
    VALUES(p_project_id,auth.uid(),'method_created',name);
    RETURN jsonb_build_object('method_id',p_method_id,'project_id',p_project_id,'display_name',name,
        'description',coalesce(p_description,''),'head_revision_id',NULL);
END;
$$;

-- Same contract as before plus immutable release notes (defaulted so older
-- callers keep working).
DROP FUNCTION public.registry_submit(uuid,uuid,text,text,text,text);
CREATE FUNCTION public.registry_submit(p_method_id uuid,p_revision_id uuid,p_parent_revision_id text,
    p_expected_head text,p_content text,p_hash text,p_release_notes text DEFAULT '') RETURNS jsonb
LANGUAGE plpgsql SECURITY DEFINER SET search_path='' AS $$
DECLARE m public.registry_methods; existing public.registry_revisions; parent uuid; payload jsonb;
    notes text := coalesce(p_release_notes,'');
BEGIN
    SELECT * INTO m FROM public.registry_methods WHERE method_id=p_method_id FOR UPDATE;
    IF NOT FOUND OR NOT registry_private.has_role(m.project_id,ARRAY['author']) THEN
        RAISE SQLSTATE '42501' USING MESSAGE='Author role required'; END IF;
    parent=NULLIF(p_parent_revision_id,'')::uuid;
    -- Idempotency is by caller-generated immutable revision ID, exact content,
    -- parent AND release notes.
    SELECT * INTO existing FROM public.registry_revisions WHERE revision_id=p_revision_id;
    IF FOUND THEN
        IF existing.method_id=p_method_id AND existing.author_id=auth.uid() AND
           existing.parent_revision_id IS NOT DISTINCT FROM parent AND
           existing.canonical_content=p_content AND existing.content_hash=p_hash AND
           existing.release_notes=notes
        THEN RETURN public.registry_fetch_revision(p_revision_id); END IF;
        RAISE SQLSTATE 'PT409' USING MESSAGE='Revision ID already used';
    END IF;
    IF m.head_revision_id IS DISTINCT FROM NULLIF(p_expected_head,'')::uuid THEN
        RAISE SQLSTATE 'PT409' USING MESSAGE='Method head changed'; END IF;
    IF parent IS NOT NULL AND NOT EXISTS(SELECT 1 FROM public.registry_revisions
        WHERE revision_id=parent AND method_id=p_method_id) THEN
        RAISE SQLSTATE 'PT409' USING MESSAGE='Parent belongs to another method'; END IF;
    IF octet_length(notes)>16384 THEN
        RAISE SQLSTATE '22023' USING MESSAGE='Release notes too long'; END IF;
    IF p_content IS NULL OR p_hash IS NULL OR octet_length(p_content)>1048576 OR
       encode(sha256(convert_to(p_content,'UTF8')),'hex')<>p_hash THEN
        RAISE SQLSTATE '22023' USING MESSAGE='Invalid content hash'; END IF;
    payload=p_content::jsonb;
    IF jsonb_typeof(payload) IS DISTINCT FROM 'object' OR
       payload->'method_schema_version' IS DISTINCT FROM '1'::jsonb OR
       jsonb_typeof(payload->'config') IS DISTINCT FROM 'object' OR
       payload->'config'->'config_schema_version' IS DISTINCT FROM '1'::jsonb OR
       jsonb_typeof(payload->'camera_script') IS DISTINCT FROM 'string' OR
       jsonb_typeof(payload->'processing_core_id') IS DISTINCT FROM 'string' OR
       length(payload->>'processing_core_id')=0 OR
       jsonb_typeof(payload->'processing_contract_version') IS DISTINCT FROM 'number' OR
       (payload->>'processing_contract_version')::numeric<1 OR
       trunc((payload->>'processing_contract_version')::numeric)<>(payload->>'processing_contract_version')::numeric OR
       jsonb_typeof(payload->'declared_hardware_compatibility') IS DISTINCT FROM 'object'
    THEN RAISE SQLSTATE '22023' USING MESSAGE='Unsupported method envelope'; END IF;
    INSERT INTO public.registry_revisions(revision_id,method_id,parent_revision_id,revision_number,
        author_id,canonical_content,content_hash,release_notes)
    VALUES(p_revision_id,p_method_id,parent,m.next_revision_number,auth.uid(),p_content,p_hash,notes);
    UPDATE public.registry_methods SET next_revision_number=next_revision_number+1 WHERE method_id=p_method_id;
    INSERT INTO public.registry_audit_events(project_id,revision_id,actor_id,action)
    VALUES(m.project_id,p_revision_id,auth.uid(),'submitted');
    RETURN public.registry_fetch_revision(p_revision_id);
END;
$$;

CREATE FUNCTION public.registry_list_methods(p_project_id uuid) RETURNS jsonb
LANGUAGE sql STABLE SECURITY INVOKER SET search_path='' AS $$
    SELECT jsonb_build_object('methods', COALESCE(jsonb_agg(jsonb_build_object(
        'method_id', m.method_id,
        'project_id', m.project_id,
        'display_name', m.display_name,
        'description', m.description,
        'head_revision_id', m.head_revision_id) ORDER BY m.display_name, m.method_id), '[]'::jsonb))
    FROM public.registry_methods m
    WHERE m.project_id = p_project_id;
$$;

CREATE FUNCTION public.registry_revision_history(p_revision_id uuid) RETURNS jsonb
LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path='' AS $$
DECLARE visible boolean;
BEGIN
    SELECT EXISTS(SELECT 1 FROM public.registry_revisions WHERE revision_id=p_revision_id) INTO visible;
    IF NOT visible THEN RAISE SQLSTATE 'PT404' USING MESSAGE='Revision not found'; END IF;
    RETURN jsonb_build_object(
        'reviews', COALESCE((SELECT jsonb_agg(jsonb_build_object(
            'reviewer_id', v.reviewer_id, 'decision', v.decision, 'reason', v.reason,
            'content_hash', v.content_hash, 'created_at', v.created_at) ORDER BY v.created_at, v.review_id)
            FROM public.registry_reviews v WHERE v.revision_id=p_revision_id), '[]'::jsonb),
        'events', COALESCE((SELECT jsonb_agg(jsonb_build_object(
            'actor_id', e.actor_id, 'action', e.action, 'reason', e.reason,
            'created_at', e.created_at) ORDER BY e.created_at, e.event_id)
            FROM public.registry_audit_events e WHERE e.revision_id=p_revision_id), '[]'::jsonb));
END;
$$;

REVOKE ALL ON FUNCTION public.registry_create_method(uuid,uuid,text,text),
    public.registry_submit(uuid,uuid,text,text,text,text,text),
    public.registry_list_methods(uuid), public.registry_revision_history(uuid)
    FROM PUBLIC,anon,authenticated;
GRANT EXECUTE ON FUNCTION public.registry_create_method(uuid,uuid,text,text),
    public.registry_submit(uuid,uuid,text,text,text,text,text),
    public.registry_list_methods(uuid), public.registry_revision_history(uuid)
    TO authenticated;
COMMIT;
