import { Button, Card, Notice } from "@mib/ui";
import { currentCompatibility, type Recipe, type RecipeCompatibility, type RecipeDraft } from "./recipeModel";

/** Pure presentation: no bridge imports, saving, scientific evaluation or hardware commands. */
export function RecipePairing({ recipe, contextRevision, compatibility, onCopy }: {
  recipe: Recipe | RecipeDraft; contextRevision: string | null; compatibility: RecipeCompatibility | null;
  onCopy?: () => void;
}) {
  const current = currentCompatibility(recipe, contextRevision, compatibility);
  return <Card title="Method + analysis recipe" subtitle={recipe.revision === null ? "Unsaved next-run draft" : `Recipe revision ${recipe.revision}`}>
    <h3>{recipe.title}</h3><p>{recipe.question || "No question recorded"}</p>
    <dl>
      <dt>Acquisition method</dt><dd>{recipe.method.id} · {recipe.method.revision} · {recipe.method.source}</dd>
      <dt>Analysis plan</dt><dd>{recipe.analysis_plan.id} · {recipe.analysis_plan.revision}</dd>
      <dt>Based on recipe</dt><dd>{recipe.lineage.recipe ? `${recipe.lineage.recipe.id} · ${recipe.lineage.recipe.revision}` : "No parent recipe"}</dd>
      <dt>Source finding</dt><dd>{recipe.lineage.finding ? `${recipe.lineage.finding.id} · ${recipe.lineage.finding.revision}` : "No finding linked"}</dd>
    </dl>
    <Notice tone={current?.status === "compatible" ? "info" : "warn"}
      title={current ? `Compatibility: ${current.status}` : "Compatibility not checked for this recipe and current setup"}>
      {current?.reasons.length ? <ul>{current.reasons.map((r, i) => <li key={i}>{r}</li>)}</ul> : null}
      Compatibility is provider evidence, not hardware verification. Copying preserves pinned references and never applies settings.
    </Notice>
    {onCopy && recipe.revision !== null ? <Button onClick={onCopy}>Base a new draft on this recipe</Button> : null}
    {recipe.revision === null ? <p role="status">Draft only · not saved or applied. Changes do not affect the active run.</p> : null}
  </Card>;
}
