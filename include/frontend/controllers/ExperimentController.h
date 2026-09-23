#pragma once

#include <QObject>
#include <cstdint>
#include <string>

#include "backend/app/PersistenceOwnership.h"

namespace backend { class AppBackend; }

namespace frontend
{

    class ExperimentController : public QObject
    {
        Q_OBJECT
    public:
        explicit ExperimentController(backend::AppBackend &backend, QObject *parent = nullptr);

        enum class State
        {
            Idle,
            Starting,
            Active,
            Stopping
        };

        State state() const { return state_; }
        bool isActive() const { return state_ == State::Active; }
        uint64_t startTimeNs() const { return startTimeNs_; }
        uint64_t endTimeNs() const { return endTimeNs_; }

        // Start experiment - returns true if started successfully
        bool startExperiment(const QString &hdf5FilePath, QString *errorMsg = nullptr);

        // Stop experiment - returns true if stopped successfully
        bool stopExperiment(QString *errorMsg = nullptr);

    signals:
        void stateChanged(State newState);
        void experimentStarted(uint64_t startTimeNs);
        void experimentStopped(uint64_t endTimeNs, size_t validFrames, size_t invalidFrames);
        void error(const QString &message);

    private:
        backend::AppBackend &backend_;
        State state_ = State::Idle;
        uint64_t startTimeNs_ = 0;
        uint64_t endTimeNs_ = 0;
        std::string hdf5FilePath_;
        // Shared HDF5 writer claim held from start until the file is closed.
        backend::app::PersistenceLease persistenceLease_;
    };

} // namespace frontend
