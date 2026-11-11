// Runs the sampling backend on its own thread so that reading /proc never
// stalls the interface. Snapshots are immutable and handed over by pointer.
#pragma once

#include "sys/sysinfo.h"

#include <QMetaType>
#include <QObject>

#include <memory>

class QTimer;

using SnapshotPtr = std::shared_ptr<const sys::Snapshot>;
Q_DECLARE_METATYPE(SnapshotPtr)

class SamplerWorker : public QObject {
    Q_OBJECT

public:
    explicit SamplerWorker(int intervalMs);
    ~SamplerWorker() override;

public slots:
    void start();                    // call once, on the worker thread
    void setInterval(int intervalMs);

signals:
    void sampled(SnapshotPtr snapshot);

private slots:
    void tick();

private:
    int m_intervalMs;
    QTimer* m_timer = nullptr;
    std::unique_ptr<sys::Sampler> m_sampler;
};
