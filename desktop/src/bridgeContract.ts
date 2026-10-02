// GENERATED FILE — do not edit by hand.
// Source of truth: crates/mib-bridge/contract/bridge-contract.json
// Regenerate with: python3 scripts/gen_bridge_contract.py
// CI verifies this file with: python3 scripts/gen_bridge_contract.py --check

export const BRIDGE_ABI_VERSION = 15;

export const EVENT_KINDS = {
  FrameReady: 0,
  CameraStatus: 1,
  RecordingStatus: 2,
  ProcessingResult: 3,
  PlaybackPosition: 4,
  BackendError: 5,
  OperationStatus: 6,
  QueueOverflow: 7,
  ExperimentStatus: 8,
} as const;

export const FRAME_READY_SOURCES = {
  LiveCapture: 0,
  Playback: 1,
  BackgroundCapture: 2,
} as const;

export const COMMAND_TYPES = {
  Camera: 0,
  Recording: 1,
  ProcessingSettings: 2,
  RecordingLoad: 3,
  PlaybackSeek: 4,
  Operation: 5,
  Experiment: 6,
  Monitoring: 7,
  Trigger: 8,
  Review: 9,
  Pump: 10,
  Autofocus: 11,
} as const;

export const CAMERA_TYPES = {
  EGrabber: 0,
  MindVision: 1,
  Mock: 2,
} as const;

export const CAMERA_SELECTION_MODES = {
  None: 0,
  Mock: 1,
  Hardware: 2,
  MindVision: 3,
} as const;

export const DISCOVERY_DEVICE_KINDS = {
  Camera: 0,
  Framegrabber: 1,
  Nanopositioner: 2,
  PulseGenerator: 3,
} as const;

export const DISCOVERY_JOB_STATES = {
  Queued: 0,
  Running: 1,
  Completed: 2,
  Cancelled: 3,
  Failed: 4,
} as const;

export const DISCOVERY_IDENTITY_STRENGTHS = {
  None: 0,
  SessionLocal: 1,
  Persistent: 2,
} as const;

export const DISCOVERY_IDENTIFICATION_STATUSES = {
  Identified: 0,
  Unidentified: 1,
  Ambiguous: 2,
  Unsupported: 3,
} as const;

export const DISCOVERY_ERROR_KINDS = {
  None: 0,
  InvalidRequest: 1,
  Busy: 2,
  OpenFailed: 3,
  PermissionDenied: 4,
  Timeout: 5,
  MalformedResponse: 6,
  Unsupported: 7,
  MissingSdk: 8,
  ProviderException: 9,
  Cancelled: 10,
  Overflow: 11,
  ShuttingDown: 12,
  TooManyJobs: 13,
} as const;

export const REVIEW_IMAGE_DATASETS = {
  ValidImage: 0,
  InvalidImage: 1,
  RecordedImage: 2,
  ValidMask: 3,
  InvalidMask: 4,
} as const;

export const OVERLAY_MODES = {
  None: 0,
  AllContour: 1,
  OuterInnerColorCoded: 2,
  AllMask: 3,
  FilteredMask: 4,
} as const;

export const REVIEW_OPERATION_KINDS = {
  ExportMetrics: 0,
  ExportAll: 1,
  BatchExport: 2,
  RegenerateMasks: 3,
  ComputeCore: 4,
  Density: 5,
  ExportCharts: 6,
} as const;

export const REVIEW_REGENERATE_SOURCES = {
  CurrentValid: 0,
  CurrentInvalid: 1,
  WholeFile: 2,
  Avi: 3,
  Folder: 4,
} as const;

export const PUMP_IDS = {
  Sample: 0,
  Sheath: 1,
} as const;

export const PUMP_RUN_STATES = {
  Stop: 0,
  Forward: 1,
  Backward: 2,
  Pause: 3,
} as const;

export const PUMP_DIRECTIONS = {
  Infuse: 0,
  Withdraw: 1,
} as const;

export const EXPERIMENT_STATES = {
  Idle: 0,
  Starting: 1,
  Active: 2,
  Stopping: 3,
  Failed: 4,
} as const;

export const EXPERIMENT_COMMAND_ACTIONS = {
  EvaluateReadiness: 0,
  Start: 1,
  Stop: 2,
  Status: 3,
} as const;

export const EXPERIMENT_START_OUTCOMES = {
  Started: 0,
  NotReady: 1,
  StaleReadiness: 2,
  AlreadyActive: 3,
  StorageFailed: 4,
  ProvenanceFailed: 5,
  Busy: 6,
} as const;

export const EXPERIMENT_STOP_OUTCOMES = {
  Accepted: 0,
  NotActive: 1,
  Busy: 2,
} as const;

export const RUN_COMPLETION_STATES = {
  Complete: 0,
  IntentionallyPartial: 1,
  IncompleteLoss: 2,
  Failed: 3,
  Unknown: 4,
} as const;

export const READINESS_GATE_STATUSES = {
  Pass: 0,
  Warn: 1,
  Fail: 2,
  Unavailable: 3,
  NotRequired: 4,
} as const;

export const ERROR_SOURCES = {
  Lifecycle: 0,
  Camera: 1,
  Recording: 2,
  Processing: 3,
  Playback: 4,
  Experiment: 5,
  Monitoring: 6,
  Hardware: 7,
  ConfigCore: 8,
  Review: 9,
  Export: 10,
  Platform: 11,
} as const;

export const OPERATION_KINDS = {
  RecordingLoad: 0,
  Experiment: 1,
  Export: 2,
  BatchMetrics: 3,
  MaskRegeneration: 4,
  Reanalysis: 5,
  PumpScan: 6,
} as const;

export const OPERATION_STATES = {
  Started: 0,
  Progress: 1,
  Completed: 2,
  Failed: 3,
  Cancelled: 4,
  TimedOut: 5,
} as const;

export const CAMERA_STATES = {
  Unconfigured: 0,
  Configured: 1,
  Starting: 2,
  Running: 3,
  Stopped: 4,
  Error: 5,
} as const;

export const RECORDING_STATES = {
  Idle: 0,
  Starting: 1,
  Recording: 2,
  Stopped: 3,
  Loaded: 4,
  Error: 5,
} as const;

export const REVIEW_DENSITY = {
  "level_count": 8,
  "grid_above_points": 5000,
  "sample_seed": 20260924,
  "grid_nx": 256,
  "grid_ny": 128,
  "ramp_rgb": [
    [
      134,
      182,
      239
    ],
    [
      109,
      167,
      236
    ],
    [
      85,
      152,
      231
    ],
    [
      57,
      135,
      229
    ],
    [
      42,
      120,
      214
    ],
    [
      37,
      106,
      191
    ],
    [
      28,
      92,
      171
    ],
    [
      24,
      79,
      149
    ],
    [
      16,
      66,
      129
    ],
    [
      13,
      54,
      107
    ]
  ],
  "ramp_semantics": "backend/processing/MonitoringDensity.h densityRampColor: linear interpolation over the stops, t in [0,1]; level k of n uses t = k/(n-1) (kdeLevelColor)"
} as const;

export const REVIEW_PIXEL_FORMATS = {
  "Mono8": 0,
  "Rgb8": 35127316
} as const;

export const FRAME_PACKET = {
  "version": 1,
  "header_bytes": 96,
  "byte_order": "little",
  "max_payload_bytes": 33554432,
  "max_pixels": 16777216,
  "max_dimension": 8192,
  "timestamp_semantics": "legacy raw timestamp_ns; unit and clock validity unavailable",
  "identity_semantics": "source/session/config unavailable; reserved u64 slots zero",
  "pull_kinds": {
    "latest": 1,
    "indexed": 2,
    "review": 3,
    "background": 4,
    "review_thumbnails": 5,
    "review_series": 6
  },
  "fields": {
    "magic": 0,
    "version": 4,
    "header_bytes": 6,
    "flags": 8,
    "pull_kind": 12,
    "frame_index": 16,
    "timestamp_ns": 24,
    "width": 32,
    "height": 40,
    "pixel_format": 48,
    "stride_bytes": 56,
    "payload_bytes": 64,
    "session_id_reserved": 72,
    "config_revision_reserved": 80,
    "source_id_reserved": 88
  },
  "pixel_formats": {
    "mono8_legacy": 0,
    "mono8": 17301505,
    "rgb8": 35127316
  },
  "rgb8_semantics": "review pulls (kinds 3, 5, 6) may carry RGB8 (3 bytes per pixel, stride = width*3) when the backend composed an overlay or ROI; mono pulls are unchanged",
  "thumbnail_strip_semantics": "pull kind 5 packs count tiles of size x size as one frame of width size and height size*count; frame_index = page offset"
} as const;

export const JSON_TRANSPORT = {
  "version": 1,
  "unsigned_integer_encoding": "canonical decimal string, 0..18446744073709551615",
  "unknown_optional_fields": "ignore",
  "unknown_required_enum": "reject and reconcile",
  "max_events": 4097,
  "max_event_text_bytes": 4096,
  "timestamp_semantics": "legacy values preserved; clock/unit/freshness unverified",
  "event_loss_semantics": "notification loss only; not acquisition loss"
} as const;

export const EVENT_PAYLOADS = {
  "FrameReady": {
    "frameIndex": "u0",
    "timestampNs": "u1",
    "width": "u2",
    "height": "u3",
    "pixelFormat": "u4",
    "strideBytes": "u5",
    "byteSize": "frame_byte_size",
    "source": "f1"
  },
  "CameraStatus": {
    "state": "u0",
    "framesProcessed": "u1",
    "frameRate": "u2",
    "dataRateMBps": "u3",
    "configured": "b0",
    "running": "b1",
    "label": "text"
  },
  "RecordingStatus": {
    "state": "u0",
    "framesWritten": "u1",
    "framesFiltered": "u2",
    "loadedValid": "u3",
    "loadedInvalid": "u4",
    "recordingFile": "b0",
    "path": "text"
  },
  "ProcessingResult": {
    "frameIndex": "u0",
    "timestampNs": "u1",
    "objectCount": "u2",
    "algorithmFps": "f0",
    "validFps": "f1",
    "invalidFps": "f2"
  },
  "PlaybackPosition": {
    "frameIndex": "u0",
    "timestampNs": "u1",
    "earliest": "u2",
    "latest": "u3",
    "available": "u4",
    "hasFrame": "b0",
    "playing": "b1"
  },
  "BackendError": {
    "source": "u0",
    "command": "u1",
    "message": "text"
  },
  "OperationStatus": {
    "operationId": "u0",
    "operationKind": "u1",
    "state": "u2",
    "progress": "u3",
    "total": "u4",
    "message": "text"
  },
  "QueueOverflow": {
    "notificationsDropped": "u0",
    "notificationsDroppedTotal": "u1"
  },
  "ExperimentStatus": {
    "state": "u0",
    "validBuffered": "u1",
    "invalidBuffered": "u2",
    "validSaved": "u3",
    "invalidSaved": "u4",
    "startTimeNs": "u5",
    "endTimeNs": "experiment_end_time_ns",
    "droppedValid": "experiment_dropped_valid",
    "droppedInvalid": "experiment_dropped_invalid",
    "flushing": "b0",
    "cancelled": "b1",
    "message": "text",
    "startGeneration": "experiment_start_generation",
    "persistenceAdmitted": "experiment_persistence_admitted",
    "persistenceCommitted": "experiment_persistence_committed",
    "persistenceFailed": "experiment_persistence_failed",
    "completion": "experiment_completion",
    "terminal": "experiment_terminal",
    "finalizationOk": "experiment_finalization_ok"
  }
} as const;

export const EVENT_KIND_NAMES: Readonly<Record<number, string>> = {
  0: "FrameReady",
  1: "CameraStatus",
  2: "RecordingStatus",
  3: "ProcessingResult",
  4: "PlaybackPosition",
  5: "BackendError",
  6: "OperationStatus",
  7: "QueueOverflow",
  8: "ExperimentStatus",
};

export type BridgeEventKindName = keyof typeof EVENT_KINDS;
