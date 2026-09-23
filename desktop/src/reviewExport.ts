/** Backend-owned transactional export. Counters and identities use decimal strings. */
export interface ReviewExportRequest {
  output_root: string;
  format: "metrics_csv" | "images" | "all";
  frames?: "valid" | "invalid" | "both";
  conversion_factor?: number;
  keep_partial_on_failure?: boolean;
  explicit_destination?: string;
}
export interface ReviewExportStatus {
  state: "idle" | "running" | "completed" | "cancelled" | "failed";
  operation_id?: string;
  job_id?: string;
  phase?: string;
  completed?: string;
  total?: string;
  current_output?: string;
  final_path?: string;
  retained_partial_path?: string;
  images_exported?: string;
  images_failed?: string;
  metrics_written?: boolean;
  warnings?: string[];
  error?: string;
}
