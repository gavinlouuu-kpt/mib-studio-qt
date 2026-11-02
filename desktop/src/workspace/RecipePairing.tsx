import { Button, Card, Notice } from "@mib/ui";
import { currentCompatibility, type Recipe, type RecipeCompatibility, type RecipeDiff, type RecipeDraft } from "./recipeModel";

/** Pure presentation: no bridge imports, saving, scientific evaluation or hardware commands.
 *  `diff` is the native recipe provider's compatibility diff (bridge ABI 50, recipe_diff), shown as
 *  the backend returned it; `compatibility` is the older per-context verdict (fixture demo). */
export function RecipePairing({ recipe, contextRevision, compatibility, diff, onCopy }: {
  recipe: Recipe | RecipeDraft; contextRevision: string | null; compatibility: RecipeCompatibility | null;
  diff?: RecipeDiff | null; onCopy?: () => void;
}) {
  const current = currentCompatibility(recipe, contextRevision, compatibility);
  const matching = diff && diff.left.id === recipe.id && diff.left.revision === recipe.revision ? diff : null;
  return <Card title="Method + analysis recipe" subtitle={recipe.revision === null ? "Unsaved next-run draft" : `Recipe revision ${recipe.revision}`}>
    <h3>{recipe.title}</h3><p>{recipe.question || "No question recorded"}</p>
    <dl>
      <dt>Acquisition method</dt><dd>{recipe.method.id} · {recipe.method.revision} · {recipe.method.source}</dd>
      <dt>Analysis plan</dt><dd>{recipe.analysis_plan.id} · {recipe.analysis_plan.revision}</dd>
      <dt>Based on recipe</dt><dd>{recipe.lineage.recipe ? `${recipe.lineage.recipe.id} · ${recipe.lineage.recipe.revision}` : "No parent recipe"}</dd>
      <dt>Source finding</dt><dd>{recipe.lineage.finding ? `${recipe.lineage.finding.id} · ${recipe.lineage.finding.revision}` : "No finding linked"}</dd>
    </dl>
    {matching ? <Notice tone={matching.overall === "equal" ? "info" : "warn"}
      title={`Compatibility with ${matching.right.kind === "instrument" ? "the instrument" : `recipe ${String(matching.right.id)} · ${String(matching.right.revision)}`}: ${matching.overall}`}>
      <ul data-testid="recipe-diff">{matching.items.map(i => <li key={i.dimension}><strong>{i.dimension}</strong> · {i.status} · {i.reason}</li>)}</ul>
      Computed by the recipe provider at this check. Copying preserves pinned references and never applies settings.
    </Notice> : <Notice tone={current?.status === "compatible" ? "info" : "warn"}
      title={current ? `Compatibility: ${current.status}` : "Compatibility not checked for this recipe and current setup"}>
      {current?.reasons.length ? <ul>{current.reasons.map((r, i) => <li key={i}>{r}</li>)}</ul> : null}
      Compatibility is provider evidence, not hardware verification. Copying preserves pinned references and never applies settings.
    </Notice>}
    {onCopy && recipe.revision !== null ? <Button onClick={onCopy}>Base a new draft on this recipe</Button> : null}
    {recipe.revision === null ? <p role="status">Draft only · not saved or applied. Changes do not affect the active run.</p> : null}
  </Card>;
}
