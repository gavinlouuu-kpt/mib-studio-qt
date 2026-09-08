#include "frontend/utils/SidebarWidget.h"

#include <QVBoxLayout>
#include <QFrame>
#include <QScrollArea>

#include "backend/app/AppBackend.h"
#include "frontend/utils/StatisticsPanel.h"
#include "frontend/utils/BackgroundPreviewWidget.h"
#include "frontend/utils/WindowGeometryPolicy.h"
#include "frontend/tabs/NanopositionerTab.h"
#include "frontend/tabs/SyringePumpTab.h"

namespace frontend
{

    SidebarWidget::SidebarWidget(backend::AppBackend& backend, QWidget* parent)
        : QWidget(parent)
        , backend_(backend)
    {
        setObjectName(QStringLiteral("hardwareSidebar"));
        setupUI();
    }

    SidebarWidget::~SidebarWidget() = default;

    void SidebarWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        contentWidget_ = new QWidget(this);
        contentLayout_ = new QVBoxLayout(contentWidget_);
        contentLayout_->setContentsMargins(0, 0, 0, 0);
        contentLayout_->setSpacing(0);

        backgroundPreview_ = new BackgroundPreviewWidget(contentWidget_);
        contentLayout_->addWidget(backgroundPreview_);

        auto* previewSeparator = new QFrame(contentWidget_);
        previewSeparator->setFrameShape(QFrame::HLine);
        previewSeparator->setFrameShadow(QFrame::Sunken);
        contentLayout_->addWidget(previewSeparator);

        statisticsPanel_ = new StatisticsPanel(contentWidget_);
        contentLayout_->addWidget(statisticsPanel_);

        auto* separator = new QFrame(contentWidget_);
        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Sunken);
        contentLayout_->addWidget(separator);

        nanopositionerTab_ = new NanopositionerTab(backend_, contentWidget_);
        contentLayout_->addWidget(nanopositionerTab_);

        auto* pumpSeparator = new QFrame(contentWidget_);
        pumpSeparator->setFrameShape(QFrame::HLine);
        pumpSeparator->setFrameShadow(QFrame::Sunken);
        contentLayout_->addWidget(pumpSeparator);

        syringePumpTab_ = new SyringePumpTab(backend_, contentWidget_);
        contentLayout_->addWidget(syringePumpTab_);

        contentLayout_->addStretch();

        // Scroll area: content scrolls in both directions, so the panel's
        // own minimum size reflects usable controls, not the widest label.
        scrollArea_ = new QScrollArea(this);
        scrollArea_->setWidget(contentWidget_);
        scrollArea_->setWidgetResizable(true);
        scrollArea_->setFrameShape(QFrame::NoFrame);
        scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea_->setMinimumWidth(0);
        mainLayout->addWidget(scrollArea_);

        // Shrinkable: the splitter decides the width; the compact floor is
        // the narrowest panel that still exposes its controls.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        setMinimumWidth(geometry::kSidebarCompactWidth);
        setMaximumWidth(geometry::kSidebarMaxWidth);
    }

    void SidebarWidget::updateBackgroundPreview(const QImage& image)
    {
        if (backgroundPreview_)
        {
            backgroundPreview_->setBackgroundImage(image);
        }
    }

} // namespace frontend
