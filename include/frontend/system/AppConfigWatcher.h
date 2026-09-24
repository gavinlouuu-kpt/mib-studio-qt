#pragma once

#include "frontend/models/ProcessingConfigDraft.h"

#include <QByteArray>
#include <QObject>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QString>
#include <QRect>

namespace backend { class AppBackend; }
namespace camera::common { enum class FrameDeliveryMode; }
class PlaybackPanel;
class QTimer;

namespace frontend {

class AppConfigWatcher : public QObject {
	Q_OBJECT
public:
	explicit AppConfigWatcher(backend::AppBackend& backend,
	                          PlaybackPanel* playbackPanel,
	                          QObject* parent = nullptr);

	// Start watching the current active app config path (external if set, else default include).
	void start();
	// Override the watched path explicitly (e.g., after Browse/Clear).
	void setWatchedPath(const QString& path);
	// Try to restore pending ROI if image dimensions are now available
	void tryRestorePendingRoi();
	// Issue #364: the one acknowledged apply/persist path for the Monitoring
	// tune panel. Validates the document (path, parse, baseline fingerprint),
	// writes only the patched keys through the checked ConfigDocumentStore
	// (unknown keys preserved), then applies the same patch to the running
	// ProcessingService and confirms it read back. Synchronous; the result
	// says exactly which of persisted / applied / conflict happened. Nothing
	// is written on conflict or validation failure.
	ConfigApplyResult applyProcessingDraft(const ApplyProcessingDraftRequest& request);
	// Persist camera.frame_delivery_mode into the watched config.json file,
	// preserving all unrelated keys.
	void writeBackCameraConfig(camera::common::FrameDeliveryMode mode);
	// Delivery mode most recently loaded from (or written to) the config file.
	camera::common::FrameDeliveryMode loadedDeliveryMode() const { return lastDeliveryMode_; }
	// Fingerprint (sha256) of the document content this watcher last loaded
	// or wrote; empty when nothing has been loaded. Self-written content is
	// recognised by this fingerprint, so the watcher echo of our own write is
	// not reloaded or re-broadcast.
	QByteArray documentFingerprint() const { return documentFingerprint_; }
	QString watchedPath() const { return watchedPath_; }

public slots:
	// Signal-friendly wrapper: runs applyProcessingDraft() and emits
	// processingDraftApplied(result).
	void onApplyProcessingDraft(const frontend::ApplyProcessingDraftRequest& request);

signals:
	// Emitted when the watched config file changes (genuine external
	// changes, and once after a successful self-write; never for the
	// watcher echo of that write).
	void configFileChanged(const QString& path);
	// Result of onApplyProcessingDraft().
	void processingDraftApplied(const frontend::ConfigApplyResult& result);
	// Emitted after camera.frame_delivery_mode has been parsed and applied to
	// CaptureService (a missing key deterministically maps to EveryFrame).
	void deliveryModeLoaded(camera::common::FrameDeliveryMode mode);

private slots:
	void onFileChanged(const QString& path);

private:
	QString resolveActiveConfigPath() const;
	void ensureDefaultConfigExists(const QString& path) const;
	void mergeNewDefaultsIntoConfig(const QString& path) const;
	void loadAndApplyFromPath(const QString& path);
	static int toOddKernelSize(int v);

	backend::AppBackend& backend_;
	PlaybackPanel* playbackPanel_{nullptr};
	QFileSystemWatcher watcher_;
	QString watchedPath_;
	QByteArray documentFingerprint_;
	QRect pendingRoi_;  // ROI to restore when image dimensions become available
	bool hasPendingRoi_ = false;  // Whether there's a pending ROI to restore
	QTimer* pendingRoiTimer_ = nullptr;  // Timer to periodically check for ROI restoration
	// Value-initialized to 0 == FrameDeliveryMode::EveryFrame (the enum is only
	// forward-declared here, so the enumerator itself is not nameable).
	camera::common::FrameDeliveryMode lastDeliveryMode_{};
};

} // namespace frontend


