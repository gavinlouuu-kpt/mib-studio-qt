import { useState } from "react";
import { useControlRole } from "../transport/useControlRole";

// Browser against the instrument: one client controls it. A second browser (or an older tab that
// reconnected first) only watches: its commands fail with VIEWER_ONLY. Say so, and offer the claim
// the server provides (take_control), instead of letting buttons fail silently.
export function ControlBanner() {
  const { viewerOnly, takeControl } = useControlRole();
  const [error, setError] = useState("");
  if (!viewerOnly) return null;
  return (
    <p className="connection-banner" role="status">
      Another client controls this instrument: you can watch, not operate.{" "}
      <button onClick={() => { setError(""); takeControl().catch((e) => setError(String(e))); }}>Take control</button>
      {error && <span role="alert"> {error}</span>}
    </p>
  );
}
