import { CAMERA_STATES, FRAME_READY_SOURCES, COMMAND_TYPES, ERROR_SOURCES, EVENT_PAYLOADS, EXPERIMENT_STATES,
  JSON_TRANSPORT, OPERATION_KINDS, OPERATION_STATES, RECORDING_STATES, READINESS_GATE_STATUSES, RUN_COMPLETION_STATES } from "./bridgeContract";
import { decimalU64 } from "./framePacket";

export function wireU64(value: unknown): string {
  if (typeof value !== "string") throw new Error("TRANSPORT_U64_MUST_BE_STRING");
  return decimalU64(value);
}
function object(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) throw new Error("TRANSPORT_INVALID_OBJECT");
  return value as Record<string, unknown>;
}
function text(value: unknown): string {
  if (typeof value !== "string") throw new Error("TRANSPORT_INVALID_TEXT");
  return value;
}
function bool(value: unknown): boolean {
  if (typeof value !== "boolean") throw new Error("TRANSPORT_INVALID_BOOLEAN");
  return value;
}
function enumValue(value: unknown, values: Readonly<Record<string, number>>): number {
  const n=Number(wireU64(value));
  if (!Object.values(values).includes(n)) throw new Error("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
  return n;
}
function version(o: Record<string, unknown>) {
  if(o.transport_version !== JSON_TRANSPORT.version) throw new Error("TRANSPORT_INCOMPATIBLE_VERSION");
}
export interface ReportedMetric {
  value: number | null;
  validity: "reported" | "invalid";
  unit: "frames/s";
  freshness: "unavailable";
}
function metric(value: unknown): ReportedMetric {
  if (value !== null && typeof value !== "number") throw new Error("TRANSPORT_INVALID_METRIC");
  return {value: typeof value === "number" && Number.isFinite(value) ? value : null,
    validity: typeof value === "number" && Number.isFinite(value) ? "reported" : "invalid",
    unit:"frames/s", freshness:"unavailable"};
}
type Message = {message:string; textTruncated:boolean};
export type BridgeEvent =
  | {kind:"FrameReady"; frameIndex:string; timestampNs:string; width:string; height:string; pixelFormat:string; strideBytes:string; byteSize:string; source:number}
  | {kind:"CameraStatus"; state:number; framesProcessed:string; frameRate:string; dataRateMBps:string; configured:boolean; running:boolean; label:string; textTruncated:boolean}
  | {kind:"RecordingStatus"; state:number; framesWritten:string; framesFiltered:string; loadedValid:string; loadedInvalid:string; recordingFile:boolean; path:string; textTruncated:boolean}
  | {kind:"ProcessingResult"; frameIndex:string; timestampNs:string; objectCount:string; algorithmFps:ReportedMetric; validFps:ReportedMetric; invalidFps:ReportedMetric}
  | {kind:"PlaybackPosition"; frameIndex:string; timestampNs:string; earliest:string; latest:string; available:string; hasFrame:boolean; playing:boolean}
  | ({kind:"BackendError"; source:number; command:number} & Message)
  | ({kind:"OperationStatus"; operationId:string; operationKind:number; state:number; progress:string; total:string} & Message)
  | {kind:"QueueOverflow"; notificationsDropped:string; notificationsDroppedTotal:string}
  | ({kind:"ExperimentStatus"; state:number; validBuffered:string; invalidBuffered:string; validSaved:string; invalidSaved:string; startTimeNs:string; endTimeNs:string; droppedValid:string; droppedInvalid:string; flushing:boolean; cancelled:boolean;
      startGeneration:string; persistenceAdmitted:string; persistenceCommitted:string; persistenceFailed:string; completion:number; terminal:boolean; finalizationOk:boolean} & Message)
  | {kind:"Unknown"; receivedKind:string; reconciliationRequired:true};

/** Only this adapter knows legacy slots. Events are notifications, never a
 * substitute for an authoritative recovery snapshot or operation query. */
export function decodeEvents(value: unknown): BridgeEvent[] {
  const envelope=object(value); version(envelope);
  if(!Array.isArray(envelope.events) || envelope.events.length>JSON_TRANSPORT.max_events)
    throw new Error("TRANSPORT_EVENT_COUNT_LIMIT");
  return envelope.events.map(raw=>{
    const e=object(raw), kind=text(e.kind);
    if(!Object.prototype.hasOwnProperty.call(EVENT_PAYLOADS,kind)) return {kind:"Unknown", receivedKind:kind, reconciliationRequired:true};
    const mapping:Readonly<Record<string,Readonly<Record<string,string>>>>=EVENT_PAYLOADS;
    const get=(field:string)=>e[mapping[kind][field]];
    const u=(field:string)=>wireU64(get(field));
    const b=(field:string)=>bool(get(field));
    const t=(field:string)=>text(get(field));
    const en=(field:string, values:Readonly<Record<string,number>>)=>enumValue(get(field),values);
    const truncated=e.text_truncated === undefined ? false : bool(e.text_truncated);
    const message=()=>({message:t("message"),textTruncated:truncated});
    if(new TextEncoder().encode(text(e.text)).length>JSON_TRANSPORT.max_event_text_bytes) throw new Error("TRANSPORT_EVENT_TEXT_LIMIT");
    switch(kind) {
      case "FrameReady": {
        const source=get("source");
        if(typeof source!=="number" || !Number.isInteger(source) || !Object.values(FRAME_READY_SOURCES).includes(source as never)) throw new Error("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
        return {kind,frameIndex:u("frameIndex"),timestampNs:u("timestampNs"),width:u("width"),height:u("height"),pixelFormat:u("pixelFormat"),strideBytes:u("strideBytes"),byteSize:u("byteSize"),source};
      }
      case "CameraStatus": return {kind,state:en("state",CAMERA_STATES),framesProcessed:u("framesProcessed"),frameRate:u("frameRate"),dataRateMBps:u("dataRateMBps"),configured:b("configured"),running:b("running"),label:t("label"),textTruncated:truncated};
      case "RecordingStatus": return {kind,state:en("state",RECORDING_STATES),framesWritten:u("framesWritten"),framesFiltered:u("framesFiltered"),loadedValid:u("loadedValid"),loadedInvalid:u("loadedInvalid"),recordingFile:b("recordingFile"),path:t("path"),textTruncated:truncated};
      case "ProcessingResult": return {kind,frameIndex:u("frameIndex"),timestampNs:u("timestampNs"),objectCount:u("objectCount"),algorithmFps:metric(get("algorithmFps")),validFps:metric(get("validFps")),invalidFps:metric(get("invalidFps"))};
      case "PlaybackPosition": return {kind,frameIndex:u("frameIndex"),timestampNs:u("timestampNs"),earliest:u("earliest"),latest:u("latest"),available:u("available"),hasFrame:b("hasFrame"),playing:b("playing")};
      case "BackendError": return {kind,source:en("source",ERROR_SOURCES),command:en("command",COMMAND_TYPES),...message()};
      case "OperationStatus": return {kind,operationId:u("operationId"),operationKind:en("operationKind",OPERATION_KINDS),state:en("state",OPERATION_STATES),progress:u("progress"),total:u("total"),...message()};
      case "QueueOverflow": return {kind,notificationsDropped:u("notificationsDropped"),notificationsDroppedTotal:u("notificationsDroppedTotal")};
      case "ExperimentStatus": return {kind,state:en("state",EXPERIMENT_STATES),validBuffered:u("validBuffered"),invalidBuffered:u("invalidBuffered"),validSaved:u("validSaved"),invalidSaved:u("invalidSaved"),startTimeNs:u("startTimeNs"),endTimeNs:u("endTimeNs"),droppedValid:u("droppedValid"),droppedInvalid:u("droppedInvalid"),flushing:b("flushing"),cancelled:b("cancelled"),
        startGeneration:u("startGeneration"),persistenceAdmitted:u("persistenceAdmitted"),persistenceCommitted:u("persistenceCommitted"),persistenceFailed:u("persistenceFailed"),completion:en("completion",RUN_COMPLETION_STATES),terminal:b("terminal"),finalizationOk:b("finalizationOk"),...message()};
      default: throw new Error("TRANSPORT_EVENT_MAPPING_MISSING");
    }
  });
}

export interface CmdResult { ok:boolean; command:number; message:string; operation_id:string }
export function decodeCommandResult(value:unknown):CmdResult {
  const o=object(value); version(o);
  if(typeof o.command!=="number" || !Object.values(COMMAND_TYPES).includes(o.command as never)) throw new Error("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
  return {ok:bool(o.ok),command:o.command,message:text(o.message),operation_id:wireU64(o.operation_id)};
}
export interface ExperimentStatus {
  valid:boolean; state:number; start_time_ns:string; end_time_ns:string;
  valid_buffered:string; invalid_buffered:string; valid_saved:string; invalid_saved:string;
  dropped_valid:string; dropped_invalid:string; flushing:boolean; cancelled:boolean; output_path:string; message:string;
  // ABI 13: the shared coordinator's full status.
  start_generation:string; readiness_generation:string; capture_generation:string;
  persistence_admitted:string; persistence_committed:string; persistence_failed:string;
  terminal:boolean; finalization_ok:boolean; completion:number; completion_reason:string; fault_code:string; fault_message:string;
}
export function decodeExperimentStatus(value:unknown):ExperimentStatus {
  const o=object(value); version(o);
  if(typeof o.state!=="number" || !Object.values(EXPERIMENT_STATES).includes(o.state as never)) throw new Error("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
  if(typeof o.completion!=="number" || !Object.values(RUN_COMPLETION_STATES).includes(o.completion as never)) throw new Error("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
  return {valid:bool(o.valid),state:o.state,start_time_ns:wireU64(o.start_time_ns),end_time_ns:wireU64(o.end_time_ns),
    valid_buffered:wireU64(o.valid_buffered),invalid_buffered:wireU64(o.invalid_buffered),valid_saved:wireU64(o.valid_saved),invalid_saved:wireU64(o.invalid_saved),
    dropped_valid:wireU64(o.dropped_valid),dropped_invalid:wireU64(o.dropped_invalid),flushing:bool(o.flushing),cancelled:bool(o.cancelled),output_path:text(o.output_path),message:text(o.message),
    start_generation:wireU64(o.start_generation),readiness_generation:wireU64(o.readiness_generation),capture_generation:wireU64(o.capture_generation),
    persistence_admitted:wireU64(o.persistence_admitted),persistence_committed:wireU64(o.persistence_committed),persistence_failed:wireU64(o.persistence_failed),
    terminal:bool(o.terminal),finalization_ok:bool(o.finalization_ok),completion:o.completion,completion_reason:text(o.completion_reason),fault_code:text(o.fault_code),fault_message:text(o.fault_message)};
}

export interface ReadinessGate { id:string; status:number; reason:string; remediation:string }
export interface ExperimentReadiness { valid:boolean; ready:boolean; generation:string; gates:ReadinessGate[] }
/** Fail (2) and Unavailable (3) block Start; unknown is never Pass. */
export function decodeExperimentReadiness(value:unknown):ExperimentReadiness {
  const o=object(value); version(o);
  if(!Array.isArray(o.gates)) throw new Error("TRANSPORT_INVALID_OBJECT");
  const gates=o.gates.map(raw=>{
    const g=object(raw);
    if(typeof g.status!=="number" || !Object.values(READINESS_GATE_STATUSES).includes(g.status as never)) throw new Error("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
    return {id:text(g.id),status:g.status,reason:text(g.reason),remediation:text(g.remediation)};
  });
  return {valid:bool(o.valid),ready:bool(o.ready),generation:wireU64(o.generation),gates};
}

export interface ProcessingStats {
  valid:boolean;
  algo_fps1s:number|null; valid_fps1s:number|null; invalid_fps1s:number|null; pixel_to_micron:number|null;
  freshness:"unavailable";
}
export function decodeProcessingStats(value:unknown):ProcessingStats {
  const o=object(value); version(o); const valid=bool(o.valid);
  const sample=(key:string)=>{
    const v=o[key];
    if(v!==null && typeof v!=="number") throw new Error("TRANSPORT_INVALID_METRIC");
    return valid && typeof v==="number" && Number.isFinite(v) ? v : null;
  };
  return {valid,algo_fps1s:sample("algo_fps1s"),valid_fps1s:sample("valid_fps1s"),invalid_fps1s:sample("invalid_fps1s"),pixel_to_micron:sample("pixel_to_micron"),freshness:"unavailable"};
}
export function formatMetric(value:number|null|undefined, digits=1):string {
  return typeof value==="number" && Number.isFinite(value) ? value.toFixed(digits) : "Unavailable";
}
