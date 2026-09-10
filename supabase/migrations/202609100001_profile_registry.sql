-- Issue #398: project-scoped registry. Auth bootstrap/membership management is
-- administrator-only in this milestone. No desktop service-role credential.
BEGIN;
CREATE SCHEMA IF NOT EXISTS registry_private;
REVOKE ALL ON SCHEMA registry_private FROM PUBLIC, anon, authenticated;

CREATE TABLE public.registry_organizations (
    organization_id uuid PRIMARY KEY DEFAULT gen_random_uuid(), display_name text NOT NULL
);
CREATE TABLE public.registry_projects (
    project_id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    organization_id uuid NOT NULL REFERENCES public.registry_organizations,
    display_name text NOT NULL
);
CREATE TABLE public.registry_memberships (
    project_id uuid NOT NULL REFERENCES public.registry_projects,
    user_id uuid NOT NULL REFERENCES auth.users,
    roles text[] NOT NULL CHECK (roles <@ ARRAY['viewer','operator','author','reviewer','publisher','admin']),
    PRIMARY KEY(project_id,user_id)
);
CREATE TABLE public.registry_methods (
    method_id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id uuid NOT NULL REFERENCES public.registry_projects,
    display_name text NOT NULL, description text NOT NULL DEFAULT '',
    head_revision_id uuid, next_revision_number bigint NOT NULL DEFAULT 1
);
CREATE TABLE public.registry_revisions (
    revision_id uuid PRIMARY KEY,
    method_id uuid NOT NULL REFERENCES public.registry_methods,
    parent_revision_id uuid REFERENCES public.registry_revisions,
    revision_number bigint NOT NULL CHECK(revision_number>0),
    author_id uuid NOT NULL REFERENCES auth.users,
    canonical_content text NOT NULL CHECK(octet_length(canonical_content)<=1048576),
    content_hash text NOT NULL CHECK(content_hash ~ '^[0-9a-f]{64}$'),
    state text NOT NULL DEFAULT 'submitted' CHECK(state IN ('submitted','approved','rejected','published','superseded','archived','revoked')),
    metadata_version bigint NOT NULL DEFAULT 1 CHECK(metadata_version>0),
    created_at timestamptz NOT NULL DEFAULT now(), published_at timestamptz,
    UNIQUE(method_id,revision_number),
    CHECK(jsonb_typeof(canonical_content::jsonb)='object'),
    CHECK((canonical_content::jsonb)->'method_schema_version'='1'::jsonb),
    CHECK(jsonb_typeof((canonical_content::jsonb)->'config')='object')
);
ALTER TABLE public.registry_methods ADD CONSTRAINT registry_head_fk FOREIGN KEY(head_revision_id) REFERENCES public.registry_revisions;
CREATE TABLE public.registry_reviews (
    review_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    revision_id uuid NOT NULL REFERENCES public.registry_revisions,
    reviewer_id uuid NOT NULL REFERENCES auth.users,
    content_hash text NOT NULL, decision text NOT NULL CHECK(decision IN ('approved','rejected')),
    reason text NOT NULL, created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE public.registry_audit_events (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    project_id uuid NOT NULL REFERENCES public.registry_projects,
    revision_id uuid REFERENCES public.registry_revisions,
    actor_id uuid REFERENCES auth.users,
    action text NOT NULL, reason text NOT NULL DEFAULT '',
    created_at timestamptz NOT NULL DEFAULT now()
);

-- Policy helpers have fixed search paths, explicit auth checks, and no dynamic SQL.
CREATE FUNCTION registry_private.has_role(p_project uuid, p_roles text[] DEFAULT NULL)
RETURNS boolean LANGUAGE sql STABLE SECURITY DEFINER SET search_path='' AS $$
    SELECT auth.uid() IS NOT NULL AND EXISTS(
        SELECT 1 FROM public.registry_memberships
        WHERE project_id=p_project AND user_id=auth.uid()
          AND (p_roles IS NULL OR roles && p_roles OR 'admin'=ANY(roles))
    );
$$;
REVOKE ALL ON FUNCTION registry_private.has_role(uuid,text[]) FROM PUBLIC;
GRANT USAGE ON SCHEMA registry_private TO authenticated;
GRANT EXECUTE ON FUNCTION registry_private.has_role(uuid,text[]) TO authenticated;

ALTER TABLE public.registry_organizations ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.registry_projects ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.registry_memberships ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.registry_methods ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.registry_revisions ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.registry_reviews ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.registry_audit_events ENABLE ROW LEVEL SECURITY;
REVOKE ALL ON public.registry_organizations,public.registry_projects,public.registry_memberships,
    public.registry_methods,public.registry_revisions,public.registry_reviews,public.registry_audit_events
    FROM PUBLIC,anon,authenticated;
GRANT SELECT ON public.registry_organizations,public.registry_projects,public.registry_memberships,
    public.registry_methods,public.registry_revisions,public.registry_reviews,public.registry_audit_events TO authenticated;
CREATE POLICY registry_org_read ON public.registry_organizations FOR SELECT TO authenticated USING(
    EXISTS(SELECT 1 FROM public.registry_projects p WHERE p.organization_id=registry_organizations.organization_id
      AND registry_private.has_role(p.project_id)));
CREATE POLICY registry_project_read ON public.registry_projects FOR SELECT TO authenticated USING(registry_private.has_role(project_id));
CREATE POLICY registry_membership_read ON public.registry_memberships FOR SELECT TO authenticated USING(user_id=auth.uid());
CREATE POLICY registry_method_read ON public.registry_methods FOR SELECT TO authenticated USING(registry_private.has_role(project_id));
CREATE POLICY registry_revision_read ON public.registry_revisions FOR SELECT TO authenticated USING(
    EXISTS(SELECT 1 FROM public.registry_methods m WHERE m.method_id=registry_revisions.method_id AND registry_private.has_role(m.project_id)));
CREATE POLICY registry_review_read ON public.registry_reviews FOR SELECT TO authenticated USING(
    EXISTS(SELECT 1 FROM public.registry_revisions r WHERE r.revision_id=registry_reviews.revision_id));
CREATE POLICY registry_audit_read ON public.registry_audit_events FOR SELECT TO authenticated USING(registry_private.has_role(project_id));

CREATE FUNCTION registry_private.immutable_revision() RETURNS trigger LANGUAGE plpgsql SET search_path='' AS $$
BEGIN
    IF TG_OP='DELETE' THEN RAISE EXCEPTION 'Historical revisions cannot be deleted'; END IF;
    IF (NEW.revision_id,NEW.method_id,NEW.parent_revision_id,NEW.revision_number,NEW.author_id,
        NEW.canonical_content,NEW.content_hash,NEW.created_at)
        IS DISTINCT FROM
       (OLD.revision_id,OLD.method_id,OLD.parent_revision_id,OLD.revision_number,OLD.author_id,
        OLD.canonical_content,OLD.content_hash,OLD.created_at)
    THEN RAISE EXCEPTION 'Revision content and identity are immutable'; END IF;
    IF OLD.state='revoked' AND NEW.state<>'revoked' THEN RAISE EXCEPTION 'Revocation is terminal'; END IF;
    RETURN NEW;
END;
$$;
CREATE TRIGGER registry_immutable BEFORE UPDATE OR DELETE ON public.registry_revisions
    FOR EACH ROW EXECUTE FUNCTION registry_private.immutable_revision();

CREATE FUNCTION public.registry_fetch_revision(p_revision_id uuid) RETURNS jsonb
LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path='' AS $$
DECLARE result jsonb;
BEGIN
    SELECT to_jsonb(r) || jsonb_build_object('project_id',m.project_id,'display_name',m.display_name)
    INTO result FROM public.registry_revisions r JOIN public.registry_methods m USING(method_id)
    WHERE r.revision_id=p_revision_id;
    IF result IS NULL THEN RAISE SQLSTATE 'PT404' USING MESSAGE='Revision not found'; END IF;
    RETURN result;
END;
$$;
CREATE FUNCTION public.registry_list_revisions(p_project_id uuid,p_after text DEFAULT '') RETURNS jsonb
LANGUAGE plpgsql STABLE SECURITY INVOKER SET search_path='' AS $$
DECLARE item public.registry_revisions; result jsonb;
BEGIN
    SELECT r.* INTO item FROM public.registry_revisions r JOIN public.registry_methods m USING(method_id)
    WHERE m.project_id=p_project_id AND (p_after='' OR r.revision_id>NULLIF(p_after,'')::uuid)
    ORDER BY r.revision_id LIMIT 1;
    IF item.revision_id IS NULL THEN RETURN jsonb_build_object('revisions','[]'::jsonb,'next_cursor',''); END IF;
    result=public.registry_fetch_revision(item.revision_id);
    RETURN jsonb_build_object('revisions',jsonb_build_array(result),'next_cursor',item.revision_id::text);
END;
$$;

CREATE FUNCTION public.registry_submit(p_method_id uuid,p_revision_id uuid,p_parent_revision_id text,
    p_expected_head text,p_content text,p_hash text) RETURNS jsonb
LANGUAGE plpgsql SECURITY DEFINER SET search_path='' AS $$
DECLARE m public.registry_methods; existing public.registry_revisions; parent uuid; payload jsonb;
BEGIN
    SELECT * INTO m FROM public.registry_methods WHERE method_id=p_method_id FOR UPDATE;
    IF NOT FOUND OR NOT registry_private.has_role(m.project_id,ARRAY['author']) THEN
        RAISE SQLSTATE '42501' USING MESSAGE='Author role required'; END IF;
    parent=NULLIF(p_parent_revision_id,'')::uuid;
    -- Idempotency is by caller-generated immutable revision ID, exact content AND parent.
    SELECT * INTO existing FROM public.registry_revisions WHERE revision_id=p_revision_id;
    IF FOUND THEN
        IF existing.method_id=p_method_id AND existing.author_id=auth.uid() AND
           existing.parent_revision_id IS NOT DISTINCT FROM parent AND
           existing.canonical_content=p_content AND existing.content_hash=p_hash
        THEN RETURN public.registry_fetch_revision(p_revision_id); END IF;
        RAISE SQLSTATE 'PT409' USING MESSAGE='Revision ID already used';
    END IF;
    IF m.head_revision_id IS DISTINCT FROM NULLIF(p_expected_head,'')::uuid THEN
        RAISE SQLSTATE 'PT409' USING MESSAGE='Method head changed'; END IF;
    IF parent IS NOT NULL AND NOT EXISTS(SELECT 1 FROM public.registry_revisions
        WHERE revision_id=parent AND method_id=p_method_id) THEN
        RAISE SQLSTATE 'PT409' USING MESSAGE='Parent belongs to another method'; END IF;
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
        author_id,canonical_content,content_hash)
    VALUES(p_revision_id,p_method_id,parent,m.next_revision_number,auth.uid(),p_content,p_hash);
    UPDATE public.registry_methods SET next_revision_number=next_revision_number+1 WHERE method_id=p_method_id;
    INSERT INTO public.registry_audit_events(project_id,revision_id,actor_id,action)
    VALUES(m.project_id,p_revision_id,auth.uid(),'submitted');
    RETURN public.registry_fetch_revision(p_revision_id);
END;
$$;
CREATE FUNCTION public.registry_transition(p_revision_id uuid,p_state text,p_expected_version bigint,p_reason text)
RETURNS jsonb LANGUAGE plpgsql SECURITY DEFINER SET search_path='' AS $$
DECLARE r public.registry_revisions; m public.registry_methods; needed text;
BEGIN
    -- All writers lock method then revision, avoiding publish/submit lock inversion.
    SELECT m0.* INTO m FROM public.registry_methods m0 JOIN public.registry_revisions r0 USING(method_id)
      WHERE r0.revision_id=p_revision_id FOR UPDATE OF m0;
    IF NOT FOUND OR NOT registry_private.has_role(m.project_id) THEN
        RAISE SQLSTATE '42501' USING MESSAGE='Project membership required'; END IF;
    SELECT * INTO r FROM public.registry_revisions WHERE revision_id=p_revision_id FOR UPDATE;
    needed=CASE WHEN p_state IN ('approved','rejected') THEN 'reviewer' ELSE 'publisher' END;
    IF NOT registry_private.has_role(m.project_id,ARRAY[needed]) THEN
        RAISE SQLSTATE '42501' USING MESSAGE='Registry role required'; END IF;
    IF p_expected_version IS DISTINCT FROM r.metadata_version THEN
        RAISE SQLSTATE 'PT409' USING MESSAGE='Metadata changed; refresh before retry'; END IF;
    IF p_reason IS NULL OR length(trim(p_reason))=0 THEN RAISE SQLSTATE '22023' USING MESSAGE='Reason required'; END IF;
    IF p_state IS NULL OR NOT (
        (r.state='submitted' AND p_state IN ('approved','rejected')) OR
        (r.state='approved' AND p_state='published') OR
        (r.state IN ('published','superseded') AND p_state IN ('archived','revoked')) OR
        (r.state='archived' AND p_state='revoked')) THEN
        RAISE SQLSTATE 'PT409' USING MESSAGE='Invalid lifecycle transition'; END IF;
    IF p_state IN ('approved','rejected') THEN
        IF r.author_id=auth.uid() THEN RAISE SQLSTATE '42501' USING MESSAGE='Independent reviewer required'; END IF;
        INSERT INTO public.registry_reviews(revision_id,reviewer_id,content_hash,decision,reason)
        VALUES(p_revision_id,auth.uid(),r.content_hash,p_state,p_reason);
    END IF;
    IF p_state='published' THEN
        IF r.parent_revision_id IS DISTINCT FROM m.head_revision_id THEN
            RAISE SQLSTATE 'PT409' USING MESSAGE='Published head changed; branch needs explicit resolution'; END IF;
        IF NOT EXISTS(SELECT 1 FROM public.registry_reviews WHERE revision_id=r.revision_id
            AND content_hash=r.content_hash AND decision='approved') THEN
            RAISE SQLSTATE '42501' USING MESSAGE='Exact hash approval required'; END IF;
        UPDATE public.registry_revisions SET state='superseded',metadata_version=metadata_version+1
        WHERE revision_id=m.head_revision_id AND state='published';
        IF FOUND THEN
            INSERT INTO public.registry_audit_events(project_id,revision_id,actor_id,action,reason)
            VALUES(m.project_id,m.head_revision_id,auth.uid(),'superseded',p_reason);
        END IF;
        UPDATE public.registry_methods SET head_revision_id=r.revision_id WHERE method_id=m.method_id;
    END IF;
    UPDATE public.registry_revisions SET state=p_state,metadata_version=metadata_version+1,
        published_at=CASE WHEN p_state='published' THEN now() ELSE published_at END
    WHERE revision_id=p_revision_id;
    INSERT INTO public.registry_audit_events(project_id,revision_id,actor_id,action,reason)
    VALUES(m.project_id,p_revision_id,auth.uid(),p_state,p_reason);
    RETURN public.registry_fetch_revision(p_revision_id);
END;
$$;
REVOKE ALL ON FUNCTION public.registry_fetch_revision(uuid), public.registry_list_revisions(uuid,text),
    public.registry_submit(uuid,uuid,text,text,text,text), public.registry_transition(uuid,text,bigint,text)
    FROM PUBLIC,anon,authenticated;
GRANT EXECUTE ON FUNCTION public.registry_fetch_revision(uuid), public.registry_list_revisions(uuid,text),
    public.registry_submit(uuid,uuid,text,text,text,text), public.registry_transition(uuid,text,bigint,text)
    TO authenticated;
COMMIT;
