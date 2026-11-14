#pragma once

#include <memory>
#include <utility>

// The queue shares one guard between the cache-load and bake stages. Only the
// final release of that guard marks unfinished work dropped. In particular, a
// temporary guard must never mark a live job dropped while it is being posted.
template <class Job> struct GeomFarLodJobGuard
{
    explicit GeomFarLodJobGuard(std::shared_ptr<Job> value) : job(std::move(value)) {}
    GeomFarLodJobGuard(const GeomFarLodJobGuard &) = delete;
    GeomFarLodJobGuard &operator=(const GeomFarLodJobGuard &) = delete;

    std::shared_ptr<Job> job;
    ~GeomFarLodJobGuard()
    {
        int running = 0;
        job->state.compare_exchange_strong(running, 3);
    }
};
