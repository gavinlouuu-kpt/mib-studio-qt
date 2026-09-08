#include "frontend/system/AppConfigWatcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QTextStream>
#include <QTimer>

#include <algorithm>

#include <spdlog/spdlog.h>
#ifdef _WIN32
#define NOMINMAX // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <shlobj.h>
#endif

#include "backend/app/AppBackend.h"
#include "backend/camera/common/ICamera.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CaptureService.h"
#include "frontend/system/ConfigDocumentStore.h"
#include "frontend/system/PlaybackPanel.h"
#include "frontend/utils/JsonConfigMerge.h"

namespace frontend
{

	namespace
	{
		// Get user-writable config directory, falling back to ../include/ for development
		static QString getUserConfigDir()
		{
			QString appDir = QCoreApplication::applicationDirPath();
			QString appDirLower = appDir.toLower();

#ifdef _WIN32
			// Check if installed in Program Files (requires admin to write)
			if (appDirLower.contains("program files") ||
				appDirLower.contains("program files (x86)"))
			{
				// Use user-writable location
				char appDataPath[MAX_PATH];
				if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath)))
				{
					QString userConfigDir = QDir(QString::fromStdString(std::string(appDataPath) + "\\MIB_Studio_Qt\\include")).absolutePath();
					// Ensure directory exists
					QDir().mkpath(userConfigDir);
					return userConfigDir;
				}
			}
#endif
			// Development: use ../include/ relative to executable
			return QDir(appDir).absoluteFilePath("../include");
		}
	}

	AppConfigWatcher::AppConfigWatcher(backend::AppBackend &backend,
									   PlaybackPanel *playbackPanel,
									   QObject *parent)
		: QObject(parent), backend_(backend), playbackPanel_(playbackPanel)
	{
		connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &AppConfigWatcher::onFileChanged);
		
		// Timer to periodically check if pending ROI can be restored
		pendingRoiTimer_ = new QTimer(this);
		pendingRoiTimer_->setInterval(500);  // Check every 500ms
		pendingRoiTimer_->setSingleShot(false);
		connect(pendingRoiTimer_, &QTimer::timeout, this, &AppConfigWatcher::tryRestorePendingRoi);
	}

	void AppConfigWatcher::start()
	{
		setWatchedPath(resolveActiveConfigPath());
	}

	void AppConfigWatcher::setWatchedPath(const QString &path)
	{
		// Remove any previous path
		if (!watchedPath_.isEmpty())
		{
			watcher_.removePath(watchedPath_);
		}
		watchedPath_.clear();

		if (path.isEmpty())
		{
			return;
		}

		ensureDefaultConfigExists(path);

		if (!watcher_.addPath(path))
		{
			SPDLOG_WARN("AppConfigWatcher: failed to watch path {}", path.toStdString());
		}
		else
		{
			SPDLOG_INFO("AppConfigWatcher: watching {}", path.toStdString());
			watchedPath_ = path;
			// Immediately apply on (re)watch
			loadAndApplyFromPath(path);
		}
	}

	void AppConfigWatcher::onFileChanged(const QString &path)
	{
		SPDLOG_INFO("AppConfigWatcher: file changed: {}", path.toStdString());
		// Some platforms require re-adding the path after change
		if (!path.isEmpty())
		{
			watcher_.removePath(path);
			watcher_.addPath(path);
		}
		// Echo of our own write (or a byte-identical external write): the
		// content is already loaded and broadcast; do not apply/reload twice
		// (issue #364).
		if (!documentFingerprint_.isEmpty())
		{
			const auto onDisk = ConfigDocumentStore::currentFingerprint(path);
			if (onDisk && *onDisk == documentFingerprint_)
			{
				SPDLOG_DEBUG("AppConfigWatcher: change event matches the loaded document; skipping reload");
				return;
			}
		}
		loadAndApplyFromPath(path);
		// Emit signal to notify other components (e.g., ConfigTabs) that file changed
		emit configFileChanged(path);
	}

	QString AppConfigWatcher::resolveActiveConfigPath() const
	{
		QSettings s;
		const QString ext = s.value("Config/ExternalAppConfigPath").toString().trimmed();
		if (!ext.isEmpty())
			return ext;
		// Use centralized helper to get user-writable config directory
		return QDir(getUserConfigDir()).absoluteFilePath("config.json");
	}

	void AppConfigWatcher::ensureDefaultConfigExists(const QString &path) const
	{
		// Only ensure when path points to app include path
		const QString defaultPath = QDir(getUserConfigDir()).absoluteFilePath("config.json");
		if (QFileInfo(path).absoluteFilePath() != QFileInfo(defaultPath).absoluteFilePath())
		{
			return;
		}
		QFileInfo fi(path);
		QDir dir(fi.absolutePath());
		if (!dir.exists())
		{
			dir.mkpath(".");
		}
		if (!QFile::exists(path))
		{
			QFile res(":/defaults/config.json");
			if (res.open(QIODevice::ReadOnly))
			{
				QFile out(path);
				if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
				{
					const QByteArray data = res.readAll();
					if (out.write(data) != data.size())
					{
						SPDLOG_WARN("AppConfigWatcher: failed to write default config.json to {}", path.toStdString());
					}
				}
				else
				{
					SPDLOG_WARN("AppConfigWatcher: failed to create {}", path.toStdString());
				}
			}
			else
			{
				SPDLOG_WARN("AppConfigWatcher: failed to open resource defaults/config.json");
			}
		}
		else
		{
			mergeNewDefaultsIntoConfig(path);
		}
	}

	void AppConfigWatcher::mergeNewDefaultsIntoConfig(const QString &path) const
	{
		// On an update, a newer build may add keys to the bundled defaults that
		// the user's existing config.json predates. Merge those missing keys in
		// (preserving every existing user value) so an updated install does not
		// drift away from a fresh install. Only the app-managed config location
		// reaches here; an external user-chosen file is never rewritten.
		QFile res(":/defaults/config.json");
		if (!res.open(QIODevice::ReadOnly))
		{
			return;
		}
		const QJsonDocument defaultsDoc = QJsonDocument::fromJson(res.readAll());
		if (!defaultsDoc.isObject())
		{
			return;
		}

		QFile in(path);
		if (!in.open(QIODevice::ReadOnly))
		{
			SPDLOG_WARN("AppConfigWatcher: failed to read config.json for default merge: {}", path.toStdString());
			return;
		}
		const QByteArray existing = in.readAll();
		in.close();

		QJsonParseError parseError{};
		const QJsonDocument userDoc = QJsonDocument::fromJson(existing, &parseError);
		if (parseError.error != QJsonParseError::NoError || !userDoc.isObject())
		{
			// Leave a malformed/unexpected file alone rather than risk clobbering it.
			SPDLOG_WARN("AppConfigWatcher: skipping default merge, config.json is not a valid object");
			return;
		}

		QJsonObject merged = userDoc.object();
		if (!jsonutil::mergeMissingDefaults(merged, defaultsDoc.object()))
		{
			return; // Already has every default key; nothing to do.
		}

		QFile out(path);
		if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			SPDLOG_WARN("AppConfigWatcher: failed to open config.json to write merged defaults: {}", path.toStdString());
			return;
		}
		const QByteArray serialized = QJsonDocument(merged).toJson(QJsonDocument::Indented);
		if (out.write(serialized) != serialized.size())
		{
			SPDLOG_WARN("AppConfigWatcher: failed to write merged config.json to {}", path.toStdString());
			return;
		}
		SPDLOG_INFO("AppConfigWatcher: merged new default keys into existing config.json at {}", path.toStdString());
	}

	int AppConfigWatcher::toOddKernelSize(int v)
	{
		if (v < 1)
			v = 1;
		if ((v % 2) == 0)
			v += 1;
		return v;
	}

	void AppConfigWatcher::loadAndApplyFromPath(const QString &path)
	{
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			SPDLOG_WARN("AppConfigWatcher: failed to open {}: {}", path.toStdString(), f.errorString().toStdString());
			return;
		}
		const QByteArray data = f.readAll();
		documentFingerprint_ = ConfigDocumentStore::fingerprintOf(data);
		// Store raw JSON for HDF5 metadata persistence
		backend_.setLastConfigJson(data.toStdString());

		const QJsonDocument doc = QJsonDocument::fromJson(data);
		if (!doc.isObject())
		{
			SPDLOG_WARN("AppConfigWatcher: config root is not an object");
			return;
		}
		const QJsonObject root = doc.object();

		// 1) Processing config
		backend::services::ProcessingConfig pcfg;
		{
			// Start from current config to preserve unspecified values
			pcfg = backend_.processing().getProcessingConfig();
			if (root.contains("image_processing") && root.value("image_processing").isObject())
			{
				const QJsonObject ip = root.value("image_processing").toObject();
				if (ip.contains("gaussian_blur_size"))
					pcfg.gaussian_blur_size = ip.value("gaussian_blur_size").toInt(pcfg.gaussian_blur_size);
				if (ip.contains("bg_subtract_threshold"))
					pcfg.bg_subtract_threshold = ip.value("bg_subtract_threshold").toInt(pcfg.bg_subtract_threshold);
				if (ip.contains("morph_kernel_size"))
					pcfg.morph_kernel_size = ip.value("morph_kernel_size").toInt(pcfg.morph_kernel_size);
				if (ip.contains("morph_iterations"))
					pcfg.morph_iterations = ip.value("morph_iterations").toInt(pcfg.morph_iterations);
				if (ip.contains("area_threshold_min"))
					pcfg.area_threshold_min = ip.value("area_threshold_min").toInt(pcfg.area_threshold_min);
				if (ip.contains("area_threshold_max"))
					pcfg.area_threshold_max = ip.value("area_threshold_max").toInt(pcfg.area_threshold_max);
				if (ip.contains("deformability_threshold_min"))
					pcfg.deformability_threshold_min = ip.value("deformability_threshold_min").toDouble(pcfg.deformability_threshold_min);
				if (ip.contains("deformability_threshold_max"))
					pcfg.deformability_threshold_max = ip.value("deformability_threshold_max").toDouble(pcfg.deformability_threshold_max);
				if (ip.contains("area_ratio_threshold_max"))
					pcfg.area_ratio_threshold_max = ip.value("area_ratio_threshold_max").toDouble(pcfg.area_ratio_threshold_max);
				if (ip.contains("ring_ratio_min"))
					pcfg.ring_ratio_min = ip.value("ring_ratio_min").toDouble(pcfg.ring_ratio_min);
				if (ip.contains("ring_ratio_max"))
					pcfg.ring_ratio_max = ip.value("ring_ratio_max").toDouble(pcfg.ring_ratio_max);
				if (ip.contains("empty_frame_pixel_threshold"))
					pcfg.empty_frame_pixel_threshold = ip.value("empty_frame_pixel_threshold").toInt(pcfg.empty_frame_pixel_threshold);
				if (ip.contains("auto_background_enabled"))
					pcfg.auto_background_enabled = ip.value("auto_background_enabled").toBool(pcfg.auto_background_enabled);
				if (ip.contains("auto_background_empty_frames"))
					pcfg.auto_background_empty_frames = ip.value("auto_background_empty_frames").toInt(pcfg.auto_background_empty_frames);
				if (ip.contains("auto_background_cooldown_frames"))
					pcfg.auto_background_cooldown_frames = ip.value("auto_background_cooldown_frames").toInt(pcfg.auto_background_cooldown_frames);
				if (ip.contains("filters") && ip.value("filters").isObject())
				{
					const QJsonObject fl = ip.value("filters").toObject();
					if (fl.contains("enable_border_check"))
						pcfg.enable_border_check = fl.value("enable_border_check").toBool(pcfg.enable_border_check);
					if (fl.contains("enable_area_range_check"))
						pcfg.enable_area_range_check = fl.value("enable_area_range_check").toBool(pcfg.enable_area_range_check);
					if (fl.contains("enable_deformability_range_check"))
						pcfg.enable_deformability_range_check = fl.value("enable_deformability_range_check").toBool(pcfg.enable_deformability_range_check);
					if (fl.contains("enable_area_ratio_check"))
						pcfg.enable_area_ratio_check = fl.value("enable_area_ratio_check").toBool(pcfg.enable_area_ratio_check);
					if (fl.contains("enable_ring_ratio_check"))
						pcfg.enable_ring_ratio_check = fl.value("enable_ring_ratio_check").toBool(pcfg.enable_ring_ratio_check);
					if (fl.contains("require_single_inner_contour"))
						pcfg.require_single_inner_contour = fl.value("require_single_inner_contour").toBool(pcfg.require_single_inner_contour);
				}
				if (ip.contains("target_group") && ip.value("target_group").isObject())
				{
					const QJsonObject tg = ip.value("target_group").toObject();
					if (tg.contains("enabled"))
						pcfg.enable_target_group = tg.value("enabled").toBool(pcfg.enable_target_group);
					if (tg.contains("area_min"))
						pcfg.target_group_area_min = tg.value("area_min").toInt(pcfg.target_group_area_min);
					if (tg.contains("area_max"))
						pcfg.target_group_area_max = tg.value("area_max").toInt(pcfg.target_group_area_max);
					if (tg.contains("deformability_min"))
						pcfg.target_group_deformability_min = tg.value("deformability_min").toDouble(pcfg.target_group_deformability_min);
					if (tg.contains("deformability_max"))
						pcfg.target_group_deformability_max = tg.value("deformability_max").toDouble(pcfg.target_group_deformability_max);
					if (tg.contains("emodulus_enabled"))
						pcfg.enable_target_group_emodulus = tg.value("emodulus_enabled").toBool(pcfg.enable_target_group_emodulus);
					if (tg.contains("emodulus_min"))
						pcfg.target_group_emodulus_min = tg.value("emodulus_min").toDouble(pcfg.target_group_emodulus_min);
					if (tg.contains("emodulus_max"))
						pcfg.target_group_emodulus_max = tg.value("emodulus_max").toDouble(pcfg.target_group_emodulus_max);
				}
				// Multi-image recording mode
				if (ip.contains("multi_image") && ip.value("multi_image").isObject())
				{
					const QJsonObject mi = ip.value("multi_image").toObject();
					if (mi.contains("enabled"))
						pcfg.multi_image_enabled = mi.value("enabled").toBool(pcfg.multi_image_enabled);
					if (mi.contains("count"))
						pcfg.multi_image_count = std::max(1, mi.value("count").toInt(pcfg.multi_image_count));
				}
			}
		}
		backend_.processing().setProcessingConfig(pcfg);
		SPDLOG_INFO("AppConfigWatcher: applied ProcessingConfig (blur={}, thresh={}, morph={}x{}, area=[{},{} um2], deform=[{:.2f},{:.2f}] enabled={}, areaRatio_max={:.2f} enabled={}, ring=[{:.1f},{:.1f}] enabled={}, empty_px={})",
					pcfg.gaussian_blur_size, pcfg.bg_subtract_threshold, pcfg.morph_kernel_size, pcfg.morph_iterations,
					pcfg.area_threshold_min, pcfg.area_threshold_max,
					pcfg.deformability_threshold_min, pcfg.deformability_threshold_max, pcfg.enable_deformability_range_check,
					pcfg.area_ratio_threshold_max, pcfg.enable_area_ratio_check,
					pcfg.ring_ratio_min, pcfg.ring_ratio_max, pcfg.enable_ring_ratio_check,
					pcfg.empty_frame_pixel_threshold);
		SPDLOG_INFO("AppConfigWatcher: target_group enabled={}, area=[{},{} um2], deform=[{:.2f},{:.2f}], emod_enabled={}, emod=[{:.1f},{:.1f}]",
					pcfg.enable_target_group, pcfg.target_group_area_min, pcfg.target_group_area_max,
					pcfg.target_group_deformability_min, pcfg.target_group_deformability_max,
					pcfg.enable_target_group_emodulus, pcfg.target_group_emodulus_min, pcfg.target_group_emodulus_max);
		if (pcfg.multi_image_enabled) {
			SPDLOG_INFO("AppConfigWatcher: multi_image enabled, count={}", pcfg.multi_image_count);
		}

		// 2) Flush interval (buffer threshold)
		if (root.contains("buffer_threshold"))
		{
			const int flushEvery = std::max(1, root.value("buffer_threshold").toInt(1000));
			backend_.processing().setFlushInterval(static_cast<size_t>(flushEvery));
		}
		// Issue #370: byte budget for the experiment buffer (MB; 0 = count-only).
		if (root.contains("experiment_buffer_max_mb"))
		{
			const double mb = std::max(0.0, root.value("experiment_buffer_max_mb").toDouble(0.0));
			backend_.processing().setMaxBufferedBytes(static_cast<uint64_t>(mb * 1024.0 * 1024.0));
		}

		// 2.25) Realtime processing mode
		if (root.contains("realtime_processing") && root.value("realtime_processing").isObject())
		{
			const QJsonObject rp = root.value("realtime_processing").toObject();
			auto batchSettings = backend_.processing().getRealtimeBatchSettings();
			if (rp.contains("batch_size"))
				batchSettings.batchSize = static_cast<size_t>(std::max(1, rp.value("batch_size").toInt(static_cast<int>(batchSettings.batchSize))));
			if (rp.contains("max_queued_frames"))
				batchSettings.maxQueuedFrames = static_cast<size_t>(std::max(1, rp.value("max_queued_frames").toInt(static_cast<int>(batchSettings.maxQueuedFrames))));
			if (rp.contains("worker_count"))
				batchSettings.workerCount = static_cast<size_t>(std::max(1, rp.value("worker_count").toInt(static_cast<int>(batchSettings.workerCount))));
			if (rp.contains("max_batch_delay_ms"))
				batchSettings.maxBatchDelayMs = std::max(1, rp.value("max_batch_delay_ms").toInt(batchSettings.maxBatchDelayMs));
			backend_.processing().setRealtimeBatchSettings(batchSettings);

			auto mode = backend_.processing().getRealtimeProcessingMode();
			if (rp.contains("mode"))
			{
				const QString modeText = rp.value("mode").toString("inline").trimmed().toLower();
				mode = (modeText == "async_batch" || modeText == "batch" || modeText == "kin6")
						   ? backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch
						   : backend::services::ProcessingService::RealtimeProcessingMode::Inline;
				backend_.processing().setRealtimeProcessingMode(mode);
			}

			SPDLOG_INFO("AppConfigWatcher: realtime_processing mode={}, batch_size={}, max_queue={}, workers={}, max_delay_ms={}",
						mode == backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch ? "async_batch" : "inline",
						batchSettings.batchSize, batchSettings.maxQueuedFrames,
						batchSettings.workerCount, batchSettings.maxBatchDelayMs);
		}

		// 2.3) Camera frame delivery mode. A missing/unknown value maps to
		// EveryFrame (deterministic migration for pre-existing profiles), so
		// the config is applied unconditionally.
		{
			QString modeText;
			if (root.contains("camera") && root.value("camera").isObject())
			{
				modeText = root.value("camera").toObject().value("frame_delivery_mode").toString();
			}
			backend::services::CaptureService::Config ccfg{};
			// bufferPartCount/numBuffers stay at the service defaults; only the
			// delivery mode is profile-configurable.
			ccfg.deliveryMode = camera::common::frameDeliveryModeFromString(modeText.toStdString());
			backend_.capture().setConfig(ccfg);
			lastDeliveryMode_ = ccfg.deliveryMode;
			SPDLOG_INFO("AppConfigWatcher: applied camera frame_delivery_mode={}",
						camera::common::toString(ccfg.deliveryMode));
			emit deliveryModeLoaded(ccfg.deliveryMode);
		}

		// 2.5) Pixel to micron conversion factor
		if (root.contains("pixel_to_micron_factor"))
		{
			const double factor = root.value("pixel_to_micron_factor").toDouble(0.4886);
			if (factor > 0.0)
			{
				backend_.processing().setPixelToMicronFactor(factor);
				SPDLOG_INFO("AppConfigWatcher: applied pixel_to_micron_factor={}", factor);
			}
		}

		// 3) Display FPS for PlaybackPanel
		if (playbackPanel_ && root.contains("display_fps"))
		{
			const int fps = std::max(1, std::min(240, root.value("display_fps").toInt(60)));
			playbackPanel_->setDisplayFps(fps);
		}

		// 4) Autofocus config (propagate to service)
		{
			backend::services::AutofocusService::Config af = backend_.autofocus().getConfig();
			if (root.contains("autofocus_focus_setpoint"))
				af.focusSetpoint = root.value("autofocus_focus_setpoint").toDouble(af.focusSetpoint);
			if (root.contains("autofocus_focus_range"))
				af.focusRange = root.value("autofocus_focus_range").toDouble(af.focusRange);
			if (root.contains("autofocus_voltage_step"))
				af.voltageStep = root.value("autofocus_voltage_step").toDouble(af.voltageStep);
			if (root.contains("autofocus_fine_voltage_step"))
				af.fineVoltageStep = root.value("autofocus_fine_voltage_step").toDouble(af.fineVoltageStep);
			if (root.contains("autofocus_max_voltage"))
				af.maxVoltage = root.value("autofocus_max_voltage").toDouble(af.maxVoltage);
			if (root.contains("autofocus_min_voltage"))
				af.minVoltage = root.value("autofocus_min_voltage").toDouble(af.minVoltage);
			if (root.contains("autofocus_initial_voltage"))
				af.initialVoltage = root.value("autofocus_initial_voltage").toDouble(af.initialVoltage);
			if (root.contains("autofocus_manual_voltage_step"))
				af.manualVoltageStep = root.value("autofocus_manual_voltage_step").toDouble(af.manualVoltageStep);
			if (root.contains("ring_ratio_stale_ms"))
				af.ringRatioStaleMs = root.value("ring_ratio_stale_ms").toInt(af.ringRatioStaleMs);
			if (root.contains("require_new_sample_per_step"))
				af.requireNewSamplePerStep = root.value("require_new_sample_per_step").toBool(af.requireNewSamplePerStep);
			if (root.contains("autofocus_min_samples_per_step"))
				af.minSamplesPerStep = root.value("autofocus_min_samples_per_step").toInt(af.minSamplesPerStep);
			if (root.contains("safe_shutdown_voltage"))
				af.safeShutdownVoltage = root.value("safe_shutdown_voltage").toDouble(af.safeShutdownVoltage);
			if (root.contains("focus_direction"))
				af.focusDirection = root.value("focus_direction").toBool(af.focusDirection);
			backend_.autofocus().setConfig(af);
		}

		// 5) ROI config (restore if valid and image dimensions match)
		if (playbackPanel_ && root.contains("roi") && root.value("roi").isObject())
		{
			const QJsonObject roiObj = root.value("roi").toObject();
			if (roiObj.contains("x") && roiObj.contains("y") && roiObj.contains("w") && roiObj.contains("h"))
			{
				const int roiX = roiObj.value("x").toInt();
				const int roiY = roiObj.value("y").toInt();
				const int roiW = roiObj.value("w").toInt();
				const int roiH = roiObj.value("h").toInt();
				
				// Validate ROI bounds against current image dimensions
				QSize imgDims = playbackPanel_->getImageDimensions();
				if (imgDims.width() > 0 && imgDims.height() > 0)
				{
					// Check if ROI is valid and within image bounds
					if (roiW > 0 && roiH > 0 && 
						roiX >= 0 && roiY >= 0 &&
						roiX + roiW <= imgDims.width() &&
						roiY + roiH <= imgDims.height())
					{
						QRect roi(roiX, roiY, roiW, roiH);
						// Only restore if different from current ROI to avoid unnecessary operations
						QRect currentRoi = playbackPanel_->getRoi();
						if (roi != currentRoi)
						{
							// Restore without saving to config to prevent infinite loop
							playbackPanel_->setRoi(roi, false);
							SPDLOG_INFO("AppConfigWatcher: restored ROI x={}, y={}, w={}, h={}", roiX, roiY, roiW, roiH);
						}
					}
					else
					{
						SPDLOG_WARN("AppConfigWatcher: ROI from config is invalid or out of bounds, ignoring");
					}
				}
				else
				{
					// Image dimensions not available yet, store ROI for later application
					pendingRoi_ = QRect(roiX, roiY, roiW, roiH);
					hasPendingRoi_ = true;
					// Start timer to periodically check if image dimensions become available
					if (pendingRoiTimer_ && !pendingRoiTimer_->isActive())
					{
						pendingRoiTimer_->start();
						SPDLOG_INFO("AppConfigWatcher: started timer to check for pending ROI restoration");
					}
					SPDLOG_INFO("AppConfigWatcher: ROI found but image dimensions not available yet, storing for later: x={}, y={}, w={}, h={}", roiX, roiY, roiW, roiH);
				}
			}
		}
		
		// Try to restore pending ROI if image dimensions are now available
		tryRestorePendingRoi();
	}
	
	void AppConfigWatcher::tryRestorePendingRoi()
	{
		if (!hasPendingRoi_ || !playbackPanel_)
		{
			// Stop timer if no pending ROI
			if (pendingRoiTimer_ && pendingRoiTimer_->isActive())
			{
				pendingRoiTimer_->stop();
			}
			return;
		}
			
		QSize imgDims = playbackPanel_->getImageDimensions();
		SPDLOG_DEBUG("AppConfigWatcher: checking pending ROI restoration, image dimensions: {}x{}", imgDims.width(), imgDims.height());
		if (imgDims.width() > 0 && imgDims.height() > 0)
		{
			const int roiX = pendingRoi_.x();
			const int roiY = pendingRoi_.y();
			const int roiW = pendingRoi_.width();
			const int roiH = pendingRoi_.height();
			
			// Check if ROI is valid and within image bounds
			if (roiW > 0 && roiH > 0 && 
				roiX >= 0 && roiY >= 0 &&
				roiX + roiW <= imgDims.width() &&
				roiY + roiH <= imgDims.height())
			{
				QRect currentRoi = playbackPanel_->getRoi();
				if (pendingRoi_ != currentRoi)
				{
					// Restore without saving to config to prevent infinite loop
					playbackPanel_->setRoi(pendingRoi_, false);
					SPDLOG_INFO("AppConfigWatcher: restored pending ROI x={}, y={}, w={}, h={}", roiX, roiY, roiW, roiH);
				}
				else
				{
					SPDLOG_DEBUG("AppConfigWatcher: pending ROI already matches current ROI, clearing pending flag");
				}
				hasPendingRoi_ = false;  // Clear pending ROI after successful restoration or if already matches
				// Stop timer after successful restoration
				if (pendingRoiTimer_ && pendingRoiTimer_->isActive())
				{
					pendingRoiTimer_->stop();
				}
			}
			else
			{
				SPDLOG_WARN("AppConfigWatcher: pending ROI is invalid or out of bounds, clearing: x={}, y={}, w={}, h={}", roiX, roiY, roiW, roiH);
				hasPendingRoi_ = false;  // Clear invalid pending ROI
				// Stop timer after clearing invalid ROI
				if (pendingRoiTimer_ && pendingRoiTimer_->isActive())
				{
					pendingRoiTimer_->stop();
				}
			}
		}
	}

	ConfigApplyResult AppConfigWatcher::applyProcessingDraft(const ApplyProcessingDraftRequest& request)
	{
		ConfigApplyResult r;
		r.requestId = request.requestId;
		r.effectiveConfig = backend_.processing().getProcessingConfig();
		const QString path = request.path.isEmpty() ? watchedPath_ : request.path;
		if (path.isEmpty())
		{
			r.error = tr("No active configuration document; nothing was changed.");
			return r;
		}
		if (request.patch.empty())
		{
			r.error = tr("Nothing to apply.");
			return r;
		}
		{
			// Validate the *result* of the patch, not just the patch, so an
			// inverted range cannot be persisted by editing one side.
			const auto merged = applyTunePatch(backend_.processing().getProcessingConfig(), request.patch);
			const QStringList issues = validateTuneConfig(merged);
			if (!issues.isEmpty())
			{
				r.error = issues.join(QStringLiteral("; "));
				return r;
			}
		}

		QFile file(path);
		if (!file.exists())
		{
			r.error = tr("Configuration file does not exist: %1").arg(path);
			return r;
		}
		if (!file.open(QIODevice::ReadOnly))
		{
			r.error = tr("Cannot read %1: %2").arg(path, file.errorString());
			return r;
		}
		const QByteArray data = file.readAll();
		file.close();
		const QByteArray onDisk = ConfigDocumentStore::fingerprintOf(data);
		if (!request.baselineFingerprint.isEmpty() && request.baselineFingerprint != onDisk)
		{
			r.conflict = true;
			r.error = tr("The configuration file changed since these values were loaded; nothing was written.");
			return r;
		}
		if (!documentFingerprint_.isEmpty() && documentFingerprint_ != onDisk && path == watchedPath_)
		{
			// An external change the watcher has not processed yet: retain it.
			r.conflict = true;
			r.error = tr("The configuration file changed on disk and has not been reloaded yet; nothing was written.");
			return r;
		}

		QJsonParseError parseError;
		QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		{
			r.error = tr("Configuration file is not valid JSON (%1); nothing was written.")
						  .arg(parseError.error == QJsonParseError::NoError ? tr("root is not an object") : parseError.errorString());
			return r;
		}
		QJsonObject root = doc.object();
		applyTunePatchToJson(root, request.patch);
		doc.setObject(root);
		const QByteArray serialized = doc.toJson(QJsonDocument::Indented);

		const ConfigWriteResult wr = ConfigDocumentStore::writeText(path, QString::fromUtf8(serialized), onDisk);
		if (!wr.ok)
		{
			r.conflict = wr.conflict;
			r.error = wr.error;
			return r;
		}
		r.persisted = true;
		r.fingerprint = wr.fingerprint;
		if (path == watchedPath_)
			documentFingerprint_ = wr.fingerprint;
		backend_.setLastConfigJson(serialized.toStdString());

		// Runtime: apply exactly the same patch, then confirm it read back.
		auto& processing = backend_.processing();
		processing.setProcessingConfig(applyTunePatch(processing.getProcessingConfig(), request.patch));
		r.effectiveConfig = processing.getProcessingConfig();
		r.applied = tunePatchSatisfiedBy(r.effectiveConfig, request.patch);
		if (!r.applied)
			r.error = tr("Saved to %1, but the processing service reports different values.").arg(path);

		SPDLOG_INFO("AppConfigWatcher: applied processing draft request {} ({} field(s)) to {}: persisted={} applied={}",
					request.requestId, request.patch.values.size(), path.toStdString(), r.persisted, r.applied);

		// One notification for the other consumers (ConfigTabs editor, the
		// Monitoring panel's baseline). Queued so the caller receives its
		// result before anyone reacts to the new document; the watcher's own
		// change event is recognised by fingerprint and not re-broadcast.
		if (path == watchedPath_)
		{
			QTimer::singleShot(0, this, [this, path]() { emit configFileChanged(path); });
		}
		return r;
	}

	void AppConfigWatcher::onApplyProcessingDraft(const frontend::ApplyProcessingDraftRequest& request)
	{
		emit processingDraftApplied(applyProcessingDraft(request));
	}

	void AppConfigWatcher::writeBackCameraConfig(camera::common::FrameDeliveryMode mode)
	{
		if (watchedPath_.isEmpty())
		{
			SPDLOG_DEBUG("AppConfigWatcher: no watched path, skipping camera write-back");
			return;
		}

		QFile file(watchedPath_);
		if (!file.exists())
		{
			SPDLOG_DEBUG("AppConfigWatcher: config file does not exist, skipping camera write-back");
			return;
		}

		if (!file.open(QIODevice::ReadOnly))
		{
			SPDLOG_WARN("AppConfigWatcher: failed to open {} for camera write-back: {}", watchedPath_.toStdString(), file.errorString().toStdString());
			return;
		}

		const QByteArray data = file.readAll();
		file.close();
		QJsonParseError parseError;
		QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		{
			SPDLOG_WARN("AppConfigWatcher: failed to parse config for camera write-back");
			return;
		}

		QJsonObject root = doc.object();
		QJsonObject cam = root.value("camera").toObject();
		cam.insert("frame_delivery_mode", QString::fromLatin1(camera::common::toString(mode)));
		root.insert("camera", cam);
		doc.setObject(root);

		const QByteArray serialized = doc.toJson(QJsonDocument::Indented);
		const ConfigWriteResult wr = ConfigDocumentStore::writeText(watchedPath_, QString::fromUtf8(serialized));
		if (!wr.ok)
		{
			SPDLOG_WARN("AppConfigWatcher: camera write-back failed: {}", wr.error.toStdString());
			return;
		}
		documentFingerprint_ = wr.fingerprint;
		backend_.setLastConfigJson(serialized.toStdString());
		lastDeliveryMode_ = mode;

		SPDLOG_INFO("AppConfigWatcher: wrote back camera frame_delivery_mode={} to {}",
					camera::common::toString(mode), watchedPath_.toStdString());
		emit configFileChanged(watchedPath_);
	}

} // namespace frontend
