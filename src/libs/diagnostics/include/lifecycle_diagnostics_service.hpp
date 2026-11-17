#pragma once
#include <memory>

namespace storm::diag
{

class LoggingService;

class LifecycleDiagnosticsService final
{
    class Guard
    {
      public:
        explicit Guard(LifecycleDiagnosticsService &inst) : inst_(inst)
        {
        }

        ~Guard()
        {
            inst_.terminate();
        }

      private:
        LifecycleDiagnosticsService &inst_;
    };

  public:
    LifecycleDiagnosticsService();
    ~LifecycleDiagnosticsService();

    [[maybe_unused, nodiscard("This guard shall exist until stack unwind")]] Guard initialize();
    void terminate() const;

  private:
    std::unique_ptr<LoggingService> loggingService_;
};

} // namespace storm::diag
