// Connect and Overview (epic #863): the Configure Mock Camera dialog. Moved from App.tsx without change (#869 wave 0).
import { open } from "../../transport/dialogs";
import { useStudio } from "../../studio/StudioContext";

export function MockCameraDialog() {
  const {
    showMockConfig, setShowMockConfig, frameDir, setFrameDir, intervalMs, setIntervalMs, loopFiles, setLoopFiles,
    onConfigureMock,
  } = useStudio();
  return (
    <>
      {showMockConfig && (
        <div className="modal-backdrop" onClick={() => setShowMockConfig(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="Configure mock camera">
            <h3>Configure Mock Camera</h3>
            <div className="row">
              <input
                type="text"
                placeholder="mock frame directory (folder of .tiff/.png)"
                value={frameDir}
                onChange={(e) => setFrameDir(e.target.value)}
              />
              <button
                className="btn"
                onClick={async () => {
                  const picked = await open({ directory: true, title: "Select mock frame directory" });
                  if (typeof picked === "string") setFrameDir(picked);
                }}
              >
                Browse…
              </button>
            </div>
            <div className="row">
              <label>
                Frame interval{" "}
                <input
                  type="number"
                  style={{ width: 80, flex: "none" }}
                  value={intervalMs}
                  onChange={(e) => setIntervalMs(e.target.value)}
                />{" "}
                ms
              </label>
              <label>
                <input type="checkbox" checked={loopFiles} onChange={(e) => setLoopFiles(e.target.checked)} /> loop files
              </label>
            </div>
            <div className="actions">
              <button className="btn" onClick={() => setShowMockConfig(false)}>Cancel</button>
              <button className="btn" onClick={onConfigureMock} disabled={!frameDir} title={frameDir ? undefined : "Pick a frame directory first"}>
                Apply
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
