#pragma once

#include <QWidget>

namespace backend { class AppBackend; }
namespace frontend { class StatisticsPanel; }
namespace frontend { class NanopositionerTab; }
namespace frontend { class SyringePumpTab; }
namespace frontend { class BackgroundPreviewWidget; }
class QScrollArea;
class QVBoxLayout;
class QImage;

namespace frontend
{

    // Hardware/statistics side panel content (issue #359).
    //
    // The widget owns its *content* only: background preview, statistics,
    // nanopositioner and syringe-pump controls inside one scroll area. It
    // does not own its width, visibility preference or persistence — the
    // main window's QSplitter is the single geometry owner and toggles the
    // panel through MainWindow::setHardwarePanelVisible().
    class SidebarWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit SidebarWidget(backend::AppBackend& backend, QWidget* parent = nullptr);
        ~SidebarWidget();

        StatisticsPanel* statisticsPanel() const { return statisticsPanel_; }
        NanopositionerTab* nanopositionerTab() const { return nanopositionerTab_; }
        SyringePumpTab* syringePumpTab() const { return syringePumpTab_; }
        QScrollArea* scrollArea() const { return scrollArea_; }

    public slots:
        void updateBackgroundPreview(const QImage& image);

    private:
        void setupUI();

        backend::AppBackend& backend_;
        BackgroundPreviewWidget* backgroundPreview_ = nullptr;
        StatisticsPanel* statisticsPanel_ = nullptr;
        NanopositionerTab* nanopositionerTab_ = nullptr;
        SyringePumpTab* syringePumpTab_ = nullptr;
        QScrollArea* scrollArea_ = nullptr;
        QWidget* contentWidget_ = nullptr;
        QVBoxLayout* contentLayout_ = nullptr;
    };

} // namespace frontend
