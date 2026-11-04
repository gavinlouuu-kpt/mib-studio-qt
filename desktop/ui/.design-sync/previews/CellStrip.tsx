import { CellStrip } from "@mib/ui";
import { illustrativeCrop } from "./illustrativeCrop";

const ids = ["R-063004", "R-063003", "R-063002", "R-063001", "R-063000", "R-062999", "R-062998", "R-062997"];
const cells = ids.map((recordId, i) => ({ recordId, ...illustrativeCrop(40 + i, 64), overlay: "contour" as const }));

export const LiveStripPinned = () => (
  <div style={{ width: 700 }}>
    <CellStrip cells={cells} pinnedId="R-063002" onPin={() => {}} />
  </div>
);

export const EmptyStrip = () => (
  <div style={{ width: 700 }}>
    <CellStrip cells={[]} empty="No cells detected yet in this run." />
  </div>
);
