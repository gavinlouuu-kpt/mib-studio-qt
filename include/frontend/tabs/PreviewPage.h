#pragma once

#include <QWidget>

#include <optional>

namespace backend { class AppBackend; }
class PlaybackPanel; // global scope
namespace frontend { class AppConfigWatcher; }
namespace frontend { class ElidingLabel; }
namespace Ui { class PreviewPage; }
class QSplitter;
class QAction;
class QActionGroup;
class QToolButton;
class QTimer;
class QShowEvent;
class QResizeEvent;
class QHideEvent;

namespace frontend {

class ConfigTabs;

// Preview workspace: live image (PlaybackPanel) above the configuration
// inspector (ConfigTabs). Carries no camera acquisition controls of its own
// (issue #360) — Start/Stop live in the application chrome and dispatch
// through MainWindow's CameraController.
//
// Issue #362: the inspector has explicit Expanded / Compact / Hidden modes
// with a stable mode bar outside the splitter, an image-biased default, a
// versioned persisted user preference (never overwritten by a temporary
// workflow override or a viewport clamp), and the QSplitter as the only
// geometry owner.
class PreviewPage : public QWidget {
    Q_OBJECT
public:
    enum class InspectorMode { Expanded, Compact, Hidden };
    static const char* toString(InspectorMode mode);
    static std::optional<InspectorMode> parseInspectorMode(const QString& text);

    explicit PreviewPage(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~PreviewPage();
    PlaybackPanel* getPlaybackPanel() const { return playback_; }
    ConfigTabs* getConfigTabs() const { return configTabs_; }
    AppConfigWatcher* getConfigWatcher() const { return configWatcher_; }

    // User preference (persisted) and the effective mode currently shown.
    void setInspectorMode(InspectorMode mode);
    InspectorMode preferredInspectorMode() const { return preferredMode_; }
    InspectorMode effectiveInspectorMode() const { return effectiveMode_; }
    // Presentation-only workflow hook: a temporary mode that does not touch
    // the saved preference; nullopt ends the override and restores it.
    void setTemporaryInspectorMode(std::optional<InspectorMode> mode);
    double preferredInspectorRatio() const { return preferredRatio_; }
    QSplitter* splitter() const;
    QAction* inspectorModeAction(InspectorMode mode) const;
    QWidget* inspectorBar() const { return inspectorBar_; }

    static constexpr int kLayoutVersion = 1;
    static constexpr double kDefaultInspectorRatio = 0.34; // image-biased seed
    static constexpr int kInspectorMinExpandedHeight = 220;
    static constexpr int kImageMinHeight = 140;

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void loadLayoutPreference();
    void saveLayoutPreference();
    void applyInspectorLayout();
    void onSplitterMoved(int pos, int index);
    void updateModeActions();
    void updateCompactSummary();
    int usableSplitterHeight() const;

    Ui::PreviewPage* ui;
    backend::AppBackend& backend_;
    PlaybackPanel* playback_ = nullptr;
    ConfigTabs* configTabs_ = nullptr;
    AppConfigWatcher* configWatcher_ = nullptr;

    QWidget* inspectorBar_ = nullptr;
    QActionGroup* modeGroup_ = nullptr;
    QAction* expandedAct_ = nullptr;
    QAction* compactAct_ = nullptr;
    QAction* hiddenAct_ = nullptr;
    QToolButton* expandedBtn_ = nullptr;
    ElidingLabel* summaryLabel_ = nullptr;
    QTimer* persistTimer_ = nullptr;
    QTimer* relayoutTimer_ = nullptr;

    InspectorMode preferredMode_{InspectorMode::Expanded};
    InspectorMode effectiveMode_{InspectorMode::Expanded};
    std::optional<InspectorMode> temporaryMode_;
    double preferredRatio_{kDefaultInspectorRatio};
    bool applyingLayout_{false};
    bool firstLayoutDone_{false};
    bool clampedForSpace_{false};
};

} // namespace frontend
