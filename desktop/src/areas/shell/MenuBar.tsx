// Shell (epic #862): the File / Settings / Help menu row. Moved from App.tsx without change (#869 wave 0).
import { useState } from "react";
import { bridge } from "../../bridge";
import { VIEWER_ONLY_REASON } from "../../components/ViewerGate";
import { offlineHelpMenu } from "../../OfflineHelp";
import { isRemote } from "../../transport";
import { openUrl, revealItemInDir } from "../../transport/dialogs";
import { useStudio } from "../../studio/StudioContext";
import { ModeToggle } from "../hardware/ServiceMode";
import { STAGE_LOCK_REASON } from "../monitoring/runStatus";

// A menu-bar dropdown. Items with a `pending` reason render disabled with the
// reason as tooltip.
function Menu(props: {
  label: string;
  items: { label: string; onClick?: () => void; pending?: string }[];
}) {
  const [openMenu, setOpenMenu] = useState(false);
  return (
    <div className="menubar-item">
      <button aria-expanded={openMenu} onClick={() => setOpenMenu((o) => !o)} onBlur={() => window.setTimeout(() => setOpenMenu(false), 150)}>
        {props.label}
      </button>
      {openMenu && (
        <div className="menu-popup" role="menu">
          {props.items.map((it) => (
            <button
              key={it.label}
              role="menuitem"
              disabled={!!it.pending}
              title={it.pending}
              onClick={() => {
                setOpenMenu(false);
                it.onClick?.();
              }}
            >
              {it.label}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

export function MenuBar() {
  const {
    openReviewFromMenu, viewerOnly, runLocked, append, requestClose, setTab, setExpTab, setConfigTab, setShowDiagnostics,
    setShowCentralMethods, openOfflineHelp, setShowAbout, setShowMonitoringSettings,
  } = useStudio();
  return (
    <nav className="menubar" aria-label="Main menu">
      <Menu
        label="File"
        items={[
          { label: "Open Recording…", onClick: openReviewFromMenu, pending: viewerOnly ? VIEWER_ONLY_REASON : runLocked ? STAGE_LOCK_REASON : undefined },
          // The browser cannot open a folder on the instrument: the item would only throw (#651 G10).
          ...(isRemote ? [] : [{
            label: "Open Data Folder",
            onClick: () => {
              void bridge
                .appPaths()
                .then((paths) => revealItemInDir(paths.app_data))
                .catch((e) => append(`open data folder failed: ${e}`));
            },
          }]),
          { label: "Exit", onClick: () => void requestClose() },
        ]}
      />
      <Menu
        label="Settings"
        items={[
          { label: "Processing Settings…", onClick: () => {setTab("experiment");setExpTab("preview");setConfigTab("app");} },
          { label: "Pixel to Micron…", onClick: () => {setTab("experiment");setExpTab("preview");setConfigTab("app");} },
          { label: "Monitoring Settings…", onClick: () => setShowMonitoringSettings(true) },
          // Board diagnostics (versions, log tail, download) are served by the instrument's server: browser only.
          ...(isRemote ? [{ label: "Diagnostics…", onClick: () => setShowDiagnostics(true) }] : []),
          // The central registry import and the application updater are desktop-shell features: in the
          // browser they would only dead-end (G10). Pixel to Micron stays (its control is on the PL too).
          ...(isRemote ? [] : [
            { label: "Central Methods…", onClick: () => setShowCentralMethods(true) },
            { label: "Updates…", onClick: () => {setTab("experiment");setExpTab("preview");setConfigTab("app");} },
          ]),
        ]}
      />
      <Menu
        label="Help"
        items={[
          ...offlineHelpMenu(openOfflineHelp),
          { label: "About", onClick: () => setShowAbout(true) },
          {
            label: "Documentation",
            onClick: () => {
              void openUrl("https://kpt1020.github.io/mib-studio-qt/").catch((e) =>
                append(`open documentation failed: ${e}`),
              );
            },
          },
          { label: "Report a Problem…", onClick: () => void openUrl("https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/new/choose").catch(e => append(`Open issue page: ${e}`)) },
        ]}
      />
      <div className="menubar-spacer" />
      <ModeToggle />
    </nav>
  );
}
