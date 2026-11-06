import { createContext, useContext } from "react";
import type { Studio } from "./useStudioController";

// The classic view's shared state (#869 wave 0). `App` builds it once with useStudioController()
// and provides it here; every area view reads what it needs with useStudio() instead of props.
export const StudioContext = createContext<Studio | null>(null);

export function useStudio(): Studio {
  const studio = useContext(StudioContext);
  if (!studio) throw new Error("useStudio() must be used inside <StudioContext.Provider> (App.tsx)");
  return studio;
}
