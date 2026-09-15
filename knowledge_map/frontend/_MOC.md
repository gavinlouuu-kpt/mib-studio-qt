# Frontend — MOC

> Qt Widgets UI. Root is [[MainWindow]]; it owns the tab widget and wires
> controllers to [[../architecture/AppBackend]].

## Core
- [[DesktopInstance]] — per-user desktop ownership before hardware initialization
- [[MainWindow]] — QMainWindow; tabs, corner widgets, sidebar, statusbar
- [[Controllers]] — CameraController, ExperimentController

## Tabs
- [[ConnectTab]] — device selection (hardware or mock)
- [[OverviewTab]] — live Mono8 display with ROI overlay and JS config editor
- [[PreviewPage]] — live display + playback + [[ConfigTabs]] dock
- [[ConfigTabs]] — experiment settings, JS camera scripts, ROI
- [[ExperimentMonitoringTab]] — live histograms + scatter plots
- [[HdfReviewTab]] — post-experiment review from saved HDF5
- [[NanopositionerTab]] — [[../services/AutofocusService]] UI
- [[SyringePumpTab]] — [[../services/SyringePumpService]] UI

## Support
- [[Dialogs]] — settings dialogs (Mock, Processing, Monitoring, Buffer save,
  Conversion factor, Frame viewer, Syringe pump)
- [[ProcessingCoreDialog]] — version history, verified cache preparation, and
  between-operation native-core activation
- [[System-Utilities]] — `AppConfigWatcher`, `AutoUpdater`,
  `DeviceInitManager`, `PlaybackPanel`, notifier bridges
- [[Screenshot-Tour]] — headless harness that regenerates the user-manual
  screenshots (`docs/manual/`)

**Up**: [[../README|Vault home]] · **See also**: [[../services/_MOC]]
