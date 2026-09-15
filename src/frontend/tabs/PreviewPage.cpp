#include "frontend/tabs/PreviewPage.h"
#include "ui_PreviewPage.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QHideEvent>
#include <QSplitter>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

#include "backend/app/AppBackend.h"
#include "frontend/system/PlaybackPanel.h"
#include "frontend/tabs/ConfigTabs.h"
#include "frontend/system/AppConfigWatcher.h"
#include "frontend/utils/ElidingLabel.h"

namespace frontend
{
    namespace {
    struct ScopedFlag {
        explicit ScopedFlag(bool& f) : flag(f) { flag = true; }
        ~ScopedFlag() { flag = false; }
        bool& flag;
    };
    constexpr const char* kKeyVersion = "Preview/LayoutVersion";
    constexpr const char* kKeyMode = "Preview/InspectorMode";
    constexpr const char* kKeyRatio = "Preview/InspectorRatio";
    } // namespace

    const char* PreviewPage::toString(InspectorMode mode)
    {
        switch (mode) {
        case InspectorMode::Expanded: return "expanded";
        case InspectorMode::Compact: return "compact";
        case InspectorMode::Hidden: return "hidden";
        }
        return "expanded";
    }

    std::optional<PreviewPage::InspectorMode> PreviewPage::parseInspectorMode(const QString& text)
    {
        const QString t = text.trimmed().toLower();
        if (t == QLatin1String("expanded")) return InspectorMode::Expanded;
        if (t == QLatin1String("compact")) return InspectorMode::Compact;
        if (t == QLatin1String("hidden")) return InspectorMode::Hidden;
        return std::nullopt;
    }

    PreviewPage::PreviewPage(backend::AppBackend &backend, QWidget *parent)
        : QWidget(parent), ui(new Ui::PreviewPage), backend_(backend)
    {
        ui->setupUi(this);

        // Live image container hosts the PlaybackPanel directly (issue #360):
        // there is no acquisition overlay covering the image any more. Camera
        // start/stop lives in the application chrome through the single
        // CameraController command path owned by MainWindow.
        auto *imageLayout = new QVBoxLayout(ui->overlayContainer);
        imageLayout->setContentsMargins(0, 0, 0, 0);
        imageLayout->setSpacing(0);
        playback_ = new PlaybackPanel(backend_, ui->overlayContainer);
        playback_->setObjectName(QStringLiteral("previewPlaybackPanel"));
        imageLayout->addWidget(playback_);
        ui->overlayContainer->setMinimumHeight(kImageMinHeight);

        // Bottom: configuration inspector. The splitter is the only geometry
        // owner (issue #362); the inspector has a real minimum in Expanded
        // mode and a header-only presentation in Compact mode — never an
        // accidental few-pixel sliver of active controls.
        configTabs_ = new ConfigTabs(backend_, this);
        configTabs_->setObjectName(QStringLiteral("previewConfigInspector"));
        configTabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        QWidget* placeholder = ui->splitter->replaceWidget(1, configTabs_);
        if (placeholder) placeholder->deleteLater();
        ui->splitter->setStretchFactor(0, 1);
        ui->splitter->setStretchFactor(1, 0);
        ui->splitter->setChildrenCollapsible(false);
        connect(ui->splitter, &QSplitter::splitterMoved, this, &PreviewPage::onSplitterMoved);

        // Stable mode bar outside the splitter: the reopen affordance never
        // disappears with the panel.
        inspectorBar_ = new QWidget(this);
        inspectorBar_->setObjectName(QStringLiteral("inspectorModeBar"));
        auto* barLayout = new QHBoxLayout(inspectorBar_);
        barLayout->setContentsMargins(6, 2, 6, 2);
        barLayout->setSpacing(4);
        barLayout->addWidget(new QLabel(tr("Settings:"), inspectorBar_));
        modeGroup_ = new QActionGroup(this);
        modeGroup_->setExclusive(true);
        auto makeAction = [this](const QString& text, const QString& name, InspectorMode mode) {
            auto* act = new QAction(text, this);
            act->setObjectName(name);
            act->setCheckable(true);
            modeGroup_->addAction(act);
            connect(act, &QAction::triggered, this, [this, mode](bool) { setInspectorMode(mode); });
            auto* btn = new QToolButton(inspectorBar_);
            btn->setDefaultAction(act);
            btn->setAutoRaise(true);
            btn->setFocusPolicy(Qt::StrongFocus);
            static_cast<QHBoxLayout*>(inspectorBar_->layout())->addWidget(btn);
            return std::make_pair(act, btn);
        };
        std::tie(expandedAct_, expandedBtn_) = makeAction(tr("Expanded"), QStringLiteral("inspectorExpandedAct"), InspectorMode::Expanded);
        std::tie(compactAct_, std::ignore) = makeAction(tr("Compact"), QStringLiteral("inspectorCompactAct"), InspectorMode::Compact);
        std::tie(hiddenAct_, std::ignore) = makeAction(tr("Hidden"), QStringLiteral("inspectorHiddenAct"), InspectorMode::Hidden);
        expandedAct_->setToolTip(tr("Show the full configuration editor below the live image"));
        compactAct_->setToolTip(tr("Show only the profile / state header below the live image"));
        hiddenAct_->setToolTip(tr("Hide the configuration inspector; the live image takes the whole page"));
        summaryLabel_ = new ElidingLabel(inspectorBar_);
        summaryLabel_->setObjectName(QStringLiteral("inspectorSummaryLabel"));
        summaryLabel_->setElideMode(Qt::ElideRight);
        summaryLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        barLayout->addWidget(summaryLabel_, 1);
        ui->verticalLayout->addWidget(inspectorBar_);
        connect(configTabs_, &ConfigTabs::documentStateChanged, this, &PreviewPage::updateCompactSummary);

        persistTimer_ = new QTimer(this);
        persistTimer_->setSingleShot(true);
        persistTimer_->setInterval(300);
        connect(persistTimer_, &QTimer::timeout, this, &PreviewPage::saveLayoutPreference);
        relayoutTimer_ = new QTimer(this);
        relayoutTimer_->setSingleShot(true);
        relayoutTimer_->setInterval(80);
        connect(relayoutTimer_, &QTimer::timeout, this, &PreviewPage::applyInspectorLayout);

        loadLayoutPreference();
        updateModeActions();
        updateCompactSummary();

        // Live config watcher: watch current path and apply changes to services and playback
        configWatcher_ = new AppConfigWatcher(backend_, playback_, this);
        connect(configTabs_, &ConfigTabs::appConfigPathChanged, configWatcher_, &AppConfigWatcher::setWatchedPath);
        // Connect file change signal to ConfigTabs to refresh JSON editor and table
        connect(configWatcher_, &AppConfigWatcher::configFileChanged, configTabs_, &ConfigTabs::onExternalConfigFileChanged);
        configWatcher_->start();
    }

    PreviewPage::~PreviewPage() {
        delete ui;
    }

    QSplitter* PreviewPage::splitter() const { return ui->splitter; }

    QAction* PreviewPage::inspectorModeAction(InspectorMode mode) const
    {
        switch (mode) {
        case InspectorMode::Expanded: return expandedAct_;
        case InspectorMode::Compact: return compactAct_;
        case InspectorMode::Hidden: return hiddenAct_;
        }
        return expandedAct_;
    }

    void PreviewPage::loadLayoutPreference()
    {
        QSettings s;
        preferredMode_ = InspectorMode::Expanded;
        preferredRatio_ = kDefaultInspectorRatio;
        if (s.value(QLatin1String(kKeyVersion), 0).toInt() != kLayoutVersion) return; // obsolete/missing: defaults
        if (const auto mode = parseInspectorMode(s.value(QLatin1String(kKeyMode)).toString())) preferredMode_ = *mode;
        bool ok = false;
        const double ratio = s.value(QLatin1String(kKeyRatio)).toDouble(&ok);
        if (ok && std::isfinite(ratio) && ratio > 0.05 && ratio < 0.95) preferredRatio_ = ratio;
    }

    void PreviewPage::saveLayoutPreference()
    {
        QSettings s;
        s.setValue(QLatin1String(kKeyVersion), kLayoutVersion);
        s.setValue(QLatin1String(kKeyMode), QLatin1String(toString(preferredMode_)));
        s.setValue(QLatin1String(kKeyRatio), preferredRatio_);
    }

    int PreviewPage::usableSplitterHeight() const
    {
        return ui->splitter->contentsRect().height() - ui->splitter->handleWidth();
    }

    void PreviewPage::setInspectorMode(InspectorMode mode)
    {
        preferredMode_ = mode;
        if (!temporaryMode_) applyInspectorLayout();
        updateModeActions();
        persistTimer_->start();
    }

    void PreviewPage::setTemporaryInspectorMode(std::optional<InspectorMode> mode)
    {
        temporaryMode_ = mode;
        applyInspectorLayout();
        updateModeActions();
    }

    void PreviewPage::updateModeActions()
    {
        const InspectorMode shown = effectiveMode_;
        for (QAction* act : {expandedAct_, compactAct_, hiddenAct_}) {
            if (!act) continue;
            const QSignalBlocker block(act);
        }
        QAction* checked = inspectorModeAction(temporaryMode_ ? shown : preferredMode_);
        if (checked) {
            const QSignalBlocker block(checked);
            checked->setChecked(true);
        }
        if (summaryLabel_) summaryLabel_->setVisible(shown != InspectorMode::Expanded);
    }

    void PreviewPage::updateCompactSummary()
    {
        if (summaryLabel_ && configTabs_) summaryLabel_->setText(configTabs_->compactSummary());
    }

    void PreviewPage::applyInspectorLayout()
    {
        if (applyingLayout_ || !configTabs_) return;
        ScopedFlag guard(applyingLayout_);
        const InspectorMode wanted = temporaryMode_.value_or(preferredMode_);
        const int usable = usableSplitterHeight();
        InspectorMode effective = wanted;
        clampedForSpace_ = false;

        if (wanted == InspectorMode::Expanded && usable > 0 &&
            usable - kImageMinHeight < kInspectorMinExpandedHeight) {
            // Not enough height for a usable editor: present the compact
            // header instead (preference untouched).
            effective = InspectorMode::Compact;
            clampedForSpace_ = true;
        }

        const bool hadFocus = configTabs_->isVisible() && configTabs_->isAncestorOf(QApplication::focusWidget());
        switch (effective) {
        case InspectorMode::Hidden:
            configTabs_->hide();
            break;
        case InspectorMode::Compact: {
            configTabs_->setCompactMode(true);
            configTabs_->setMinimumHeight(0);
            configTabs_->show();
            const int headerH = configTabs_->headerWidget() ? configTabs_->headerWidget()->sizeHint().height() + 6 : 48;
            configTabs_->setMaximumHeight(headerH);
            if (usable > 0) ui->splitter->setSizes({std::max(kImageMinHeight, usable - headerH), headerH});
            break;
        }
        case InspectorMode::Expanded: {
            configTabs_->setCompactMode(false);
            configTabs_->setMaximumHeight(QWIDGETSIZE_MAX);
            // No hard minimum (that would become outer-window pressure, #358):
            // the usable minimum is enforced by this layout policy and by the
            // drag clamp in onSplitterMoved().
            configTabs_->setMinimumHeight(0);
            configTabs_->show();
            if (usable > 0) {
                const int wantedH = static_cast<int>(std::lround(preferredRatio_ * usable));
                const int maxH = usable - kImageMinHeight;
                const int h = std::clamp(wantedH, kInspectorMinExpandedHeight, std::max(kInspectorMinExpandedHeight, maxH));
                ui->splitter->setSizes({usable - h, h});
            }
            break;
        }
        }
        if (hadFocus && effective != InspectorMode::Expanded && expandedBtn_) expandedBtn_->setFocus(Qt::OtherFocusReason);
        effectiveMode_ = effective;
        updateModeActions();
        updateCompactSummary();
    }

    void PreviewPage::onSplitterMoved(int pos, int index)
    {
        Q_UNUSED(pos);
        Q_UNUSED(index);
        if (applyingLayout_ || temporaryMode_ || effectiveMode_ != InspectorMode::Expanded) return;
        const int usable = usableSplitterHeight();
        QList<int> sizes = ui->splitter->sizes();
        if (usable <= 0 || sizes.size() < 2) return;
        if (sizes[1] < kInspectorMinExpandedHeight) {
            // Deliberate clamp: an expanded editor never becomes a sliver.
            ScopedFlag guard(applyingLayout_);
            const int h = std::min(kInspectorMinExpandedHeight, std::max(0, usable - kImageMinHeight));
            sizes = {usable - h, h};
            ui->splitter->setSizes(sizes);
        }
        preferredRatio_ = std::clamp(static_cast<double>(sizes[1]) / usable, 0.1, 0.9);
        persistTimer_->start();
    }

    void PreviewPage::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        if (firstLayoutDone_) return;
        firstLayoutDone_ = true;
        QTimer::singleShot(0, this, [this]() { applyInspectorLayout(); });
    }

    void PreviewPage::hideEvent(QHideEvent* event)
    {
        // Flush a pending (debounced) preference write so closing the page or
        // the application never loses the last drag/mode change.
        if (persistTimer_ && persistTimer_->isActive()) {
            persistTimer_->stop();
            saveLayoutPreference();
        }
        QWidget::hideEvent(event);
    }

    void PreviewPage::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (firstLayoutDone_ && !applyingLayout_ && relayoutTimer_) relayoutTimer_->start();
    }

} // namespace frontend
