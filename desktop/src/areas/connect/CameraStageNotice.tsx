// Connect and Overview (epic #863, bug #846): what entering a stage did to the camera, and why the camera
// window could not be read or saved, shown in the stage it concerns instead of only in the log.
import { useStudio } from "../../studio/StudioContext";
import type { StageName } from "./cameraStage";

export function CameraStageNotice({ stage }: { stage: StageName }) {
  const { stageNotice, geometryError, cameraWin } = useStudio();
  const lines: Array<{ level: "info" | "warn" | "error"; text: string }> = [];
  if (stageNotice?.stage === stage) lines.push(stageNotice);
  if (stage === "overview") {
    if (geometryError) lines.push({ level: "error", text: geometryError });
    if (cameraWin.notice) lines.push(cameraWin.notice);
  }
  if (!lines.length) return null;
  return (
    <>
      {lines.map((line) => (
        <div key={`${line.level}:${line.text}`} className={`stage-notice ${line.level}`} role={line.level === "error" ? "alert" : "status"} data-testid="camera-stage-notice">
          {line.text}
        </div>
      ))}
    </>
  );
}
