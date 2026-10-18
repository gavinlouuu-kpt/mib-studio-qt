import { Stepper } from "@mib/ui";

export const RunLifecycle = () => (
  <Stepper
    steps={[
      { label: "Start requested", state: "done" },
      { label: "Acknowledged · first frame seen", state: "done" },
      { label: "Active · recording", state: "done" },
      { label: "Stopping", state: "done" },
      { label: "Drain · finalizing", state: "done" },
      { label: "Confirmed closed", state: "done" },
    ]}
  />
);

export const ExportJob = () => (
  <Stepper
    steps={[
      { label: "Queued", state: "done" },
      { label: "Writing 3 of 7", state: "current" },
      { label: "Verifying checksums", state: "pending" },
      { label: "Done · 7 files · exports.json", state: "pending" },
    ]}
  />
);

export const Failed = () => (
  <Stepper
    steps={[
      { label: "Plan r4 submitted", state: "done" },
      { label: "Loaded 7 files (read-only)", state: "done" },
      { label: "Populations evaluated · worker stopped", state: "failed" },
      { label: "Statistics written", state: "pending" },
    ]}
  />
);
