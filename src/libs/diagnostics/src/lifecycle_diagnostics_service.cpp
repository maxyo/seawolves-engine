#include "lifecycle_diagnostics_service.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "fs.h"
#include "v_file_service.h"
#include "spdlog_sinks/syncable_sink.hpp"
#include "watermark.hpp"

namespace storm::diag
{

class LoggingService final
{
  public:
    ~LoggingService()
    {
        terminate();
    }

    void initialize()
    {
        if (terminate_)
        {
            terminate_ = false;

            static auto terminate_handler = std::get_terminate();
            std::set_terminate([] {
                terminate_handler();
            });

            create_directories(fs::GetLogsPath());

            std::thread worker{[this] { loggingThread(); }};
            worker.detach();
        }
    }

    void terminate()
    {
        using namespace std::chrono_literals;

        if (!terminate_)
        {
            terminate_ = true;
            {
                std::lock_guard lock(mtx_);
                flushRequested_ = true;
            }
            cv_.notify_one();

            std::unique_lock lock(mtx_);
            cv_.wait_for(lock, 5s, [this] { return !flushRequested_; });
            spdlog::shutdown();
        }
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    bool flushRequested_{false};
    std::atomic_bool terminate_{true};

    void flushAll(const bool terminate) const
    {
        spdlog::apply_all([terminate](std::shared_ptr<spdlog::logger> l) {
            l->flush();

            if (terminate)
            {
                for (auto &sink : l->sinks())
                {
                    if (const auto syncable_sink = std::dynamic_pointer_cast<logging::sinks::syncable_sink>(sink))
                    {
                        syncable_sink->terminate_immediately();
                    }
                }
            }
        });
    }

    void loggingThread()
    {
        while (!terminate_)
        {
            std::unique_lock lock(mtx_);
            cv_.wait(lock, [this] { return flushRequested_; });

            flushAll(terminate_);

            flushRequested_ = false;
            cv_.notify_one();
        }
    }
};

LifecycleDiagnosticsService::LifecycleDiagnosticsService() : loggingService_(std::make_unique<LoggingService>())
{
}

LifecycleDiagnosticsService::~LifecycleDiagnosticsService()
{
    terminate();
}

LifecycleDiagnosticsService::Guard LifecycleDiagnosticsService::initialize()
{
    loggingService_->initialize();
    return Guard(*this);
}

void LifecycleDiagnosticsService::terminate() const
{
    loggingService_->terminate();
}

} // namespace storm::diag
