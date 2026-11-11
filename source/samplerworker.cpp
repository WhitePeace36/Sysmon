#include "samplerworker.h"

#include <QTimer>

SamplerWorker::SamplerWorker(int intervalMs) : m_intervalMs(intervalMs) {}

SamplerWorker::~SamplerWorker() = default;

void SamplerWorker::start()
{
    m_sampler = std::make_unique<sys::Sampler>();
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SamplerWorker::tick);
    m_timer->start(m_intervalMs);

    // The first sample only establishes the baseline for all rates, so take
    // a second one shortly after instead of showing zeros for a full interval.
    tick();
    QTimer::singleShot(300, this, &SamplerWorker::tick);
}

void SamplerWorker::setInterval(int intervalMs)
{
    m_intervalMs = intervalMs;
    if (m_timer)
        m_timer->start(m_intervalMs);
}

void SamplerWorker::tick()
{
    if (!m_sampler)
        return;
    SnapshotPtr snapshot = m_sampler->sample();
    emit sampled(snapshot);
}
