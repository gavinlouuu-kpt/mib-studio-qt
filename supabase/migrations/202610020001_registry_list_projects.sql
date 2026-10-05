-- Issue #398 M1: project discovery for the desktop registry worker. Returns
-- only the caller's own memberships (SECURITY INVOKER: RLS applies), so a
-- refresh can cover every project the user belongs to without a table call.
BEGIN;
CREATE FUNCTION public.registry_list_projects() RETURNS jsonb
LANGUAGE sql STABLE SECURITY INVOKER SET search_path='' AS $$
    SELECT jsonb_build_object('projects', COALESCE(jsonb_agg(jsonb_build_object(
        'project_id', p.project_id,
        'display_name', p.display_name,
        'roles', to_jsonb(mb.roles)) ORDER BY p.display_name, p.project_id), '[]'::jsonb))
    FROM public.registry_memberships mb
    JOIN public.registry_projects p USING(project_id)
    WHERE mb.user_id = auth.uid();
$$;
REVOKE ALL ON FUNCTION public.registry_list_projects() FROM PUBLIC, anon, authenticated;
GRANT EXECUTE ON FUNCTION public.registry_list_projects() TO authenticated;
COMMIT;
