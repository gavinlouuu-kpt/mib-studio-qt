import { stripView, type SsdStatus } from "../ssdView";

// The storage strip (#667 S1): what the SATA SSD is doing and why, in one line, with the recovery note kept below it.
export function SsdStrip({ status }: { status: SsdStatus | null }) {
  if (!status) return null;
  const v = stripView(status);
  return (
    <div className={`ssd-strip ${v.tone}`} data-testid="ssd-strip" role="status">
      <span data-testid="ssd-strip-text">{v.text}</span>
      {v.detail && <div className="pending-note" data-testid="ssd-strip-detail">{v.detail}</div>}
    </div>
  );
}
