import { Button, Notice } from "@mib/ui";

export const SettingsDiffer = () => (
  <div style={{ width: 680 }}>
    <Notice
      tone="warn"
      title="2 settings differ from method Cell mechanics / v3"
      actions={
        <>
          <Button>Save to profile</Button>
          <Button>Revert</Button>
        </>
      }
    >
      Exposure 2.0 µs (method 3.0) · Frame rate 5 000 fps (method 2 000). Recorded in every run header.
    </Notice>
  </div>
);

export const StartBlocked = () => (
  <div style={{ width: 680 }}>
    <Notice tone="blocked" title="Start blocked · 1 gate" actions={<Button>Focus setup</Button>}>
      Focus: coarse motor origin unknown. Run focus setup in Prepare.
    </Notice>
  </div>
);

export const ReadingUnknown = () => (
  <div style={{ width: 680 }}>
    <Notice tone="unknown" title="Reservoir level unknown">
      No level sensor is fitted; delivered volume is an estimate from pump speed.
    </Notice>
  </div>
);

export const RunClosed = () => (
  <div style={{ width: 680 }}>
    <Notice tone="confirmed" title="Run closed · Exp-0142_03 verified" actions={<Button variant="primary">Next sample</Button>}>
      Writer closed and checksums verified 4:21 after Start.
    </Notice>
  </div>
);
