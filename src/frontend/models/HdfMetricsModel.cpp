#include "frontend/models/HdfMetricsModel.h"

#include "backend/processing/ProcessingService.h"

#include <cmath>

namespace frontend {

HdfMetricsModel::HdfMetricsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void HdfMetricsModel::setSource(const std::vector<backend::services::ProcessedFrame>* framesPtr) {
    beginResetModel();
    frames_ = framesPtr;
    endResetModel();
}

void HdfMetricsModel::setPixelToMicronFactor(double factor) {
    beginResetModel();
    pixelToMicronFactor_ = factor;
    endResetModel();
}

int HdfMetricsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return frames_ ? static_cast<int>(frames_->size()) : 0;
}

int HdfMetricsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return 24;
}

QVariant HdfMetricsModel::data(const QModelIndex& index, int role) const {
    if (!frames_ || !index.isValid() || role != Qt::DisplayRole) return {};
    const auto& frame = frames_->at(static_cast<size_t>(index.row()));
    const auto& val = frame.validation;
    switch (index.column()) {
        case 0:  return static_cast<qulonglong>(frame.index);
        case 1:  return static_cast<qulonglong>(frame.timestampNs);
        case 2:  return val.objectId;
        case 3:  return val.objectCount;
        case 4:  return val.trackId;
        case 5:  return static_cast<qulonglong>(val.trackFirstFrame);
        case 6:  return static_cast<qulonglong>(val.trackLastFrame);
        case 7:  return val.trackObservationCount;
        case 8:  return QString::number(val.deformability, 'f', 3);
        case 9:  return QString::number(val.area, 'f', 2);
        case 10: return QString::number(val.area * pixelToMicronFactor_ * pixelToMicronFactor_, 'f', 2);
        case 11: return QString::number(val.areaRatio, 'f', 3);
        case 12: return QString::number(val.ringRatio, 'f', 3);
        case 13: return val.isValid ? "Yes" : "No";
        case 14: return val.touchesBorder ? "Yes" : "No";
        case 15: return val.hasSingleInnerContour ? "Yes" : "No";
        case 16: return val.inRange ? "Yes" : "No";
        case 17: return val.innerContourCount;
        case 18: return QString::number(val.brightness.q1, 'f', 2);
        case 19: return QString::number(val.brightness.q2, 'f', 2);
        case 20: return QString::number(val.brightness.q3, 'f', 2);
        case 21: return QString::number(val.brightness.q4, 'f', 2);
        case 22: return val.isTargetGroup ? "Yes" : "No";
        case 23: return std::isnan(val.youngsModulus) ? QString() : QString::number(val.youngsModulus, 'f', 3);
        default: return {};
    }
}

QVariant HdfMetricsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Horizontal) {
        switch (section) {
            case 0:  return "Index";
            case 1:  return "Timestamp";
            case 2:  return "Object Id";
            case 3:  return "Object Count";
            case 4:  return "Track Id";
            case 5:  return "Track First";
            case 6:  return "Track Last";
            case 7:  return "Track Observations";
            case 8:  return "Deformability";
            case 9:  return "Area (px)";
            case 10: return QString::fromUtf8("Area (\xc2\xb5m\xc2\xb2)");
            case 11: return "Area Ratio";
            case 12: return "Ring Ratio";
            case 13: return "Valid";
            case 14: return "Touches Border";
            case 15: return "Single Inner";
            case 16: return "In Range";
            case 17: return "Inner Count";
            case 18: return "Bright Q1";
            case 19: return "Bright Q2";
            case 20: return "Bright Q3";
            case 21: return "Bright Q4";
            case 22: return "Target Group";
            case 23: return "Young's modulus (kPa)";
            default: return {};
        }
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

Qt::ItemFlags HdfMetricsModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

} // namespace frontend





