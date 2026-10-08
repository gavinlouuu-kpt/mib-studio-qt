#pragma once

#include <QDialog>
#include <QString>

namespace frontend {

QString helpContentDirectory(const QString& name);
QString bundledReleaseNotes(const QString& directory, const QString& version);
bool shouldShowWhatsNew(const QString& previous, const QString& current);

class HelpDialog final : public QDialog {
public:
    explicit HelpDialog(bool manual, QWidget* parent = nullptr, const QString& directory = {});
};

} // namespace frontend
