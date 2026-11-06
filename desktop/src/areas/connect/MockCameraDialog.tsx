// Connect and Overview (epic #863): the Configure Mock Camera dialog, as Qt's MockConfigDialog: a frame folder (trimmed) and a
// frame rate in fps (1 to 10000, default 5000, the rig's rate). The fields start from what was used last time.
import { open } from "../../transport/dialogs";
import { useStudio } from "../../studio/StudioContext";
import { MOCK_FPS_MAX, MOCK_FPS_MIN, mockFpsProblem } from "./connectFlow";

export function MockCameraDialog() {
  const {
    showMockConfig, setShowMockConfig, frameDir, setFrameDir, mockFps, setMockFps, loopFiles, setLoopFiles, onConfigureMock,
  } = useStudio();
  const fpsProblem = mockFpsProblem(mockFps);
  const applyBlock = !frameDir.trim() ? "Pick a frame directory first" : fpsProblem;
  return (
    <>
      {showMockConfig && (
        <div className="modal-backdrop" onClick={() => setShowMockConfig(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="Configure mock camera">
            <h3>Configure Mock Camera</h3>
            <div className="row">
              <input
                type="text"
                aria-label="Mock frame directory"
                placeholder="mock frame directory (folder of .tiff/.png)"
                value={frameDir}
                onChange={(e) => setFrameDir(e.target.value)}
              />
              <button
                className="btn"
                onClick={async () => {
                  const picked = await open({ directory: true, title: "Select mock frame directory", defaultPath: frameDir.trim() || undefined });
                  if (typeof picked === "string") setFrameDir(picked.trim());
                }}
              >
                Browse…
              </button>
            </div>
            <div className="row">
              <label>
                Frame rate{" "}
                <input
                  type="number"
                  aria-label="Mock frame rate (fps)"
                  aria-invalid={!!fpsProblem}
                  min={MOCK_FPS_MIN}
                  max={MOCK_FPS_MAX}
                  step={1}
                  style={{ width: 80, flex: "none" }}
                  value={mockFps}
                  onChange={(e) => setMockFps(e.target.value)}
                />{" "}
                fps
              </label>
              <label>
                <input type="checkbox" checked={loopFiles} onChange={(e) => setLoopFiles(e.target.checked)} /> loop files
              </label>
            </div>
            {fpsProblem && <p role="alert">{fpsProblem}</p>}
            <div className="actions">
              <button className="btn" onClick={() => setShowMockConfig(false)}>Cancel</button>
              <button className="btn" onClick={onConfigureMock} disabled={!!applyBlock} title={applyBlock}>
                Apply
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
