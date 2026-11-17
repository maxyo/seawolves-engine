#include "background_read.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#endif

namespace storm::io
{
namespace
{
std::mutex g_doneMutex;
std::condition_variable g_doneSignal;
} // namespace

class BackgroundReader
{
  public:
    static BackgroundReader &instance()
    {
        static BackgroundReader reader;
        return reader;
    }

    void push(std::shared_ptr<BackgroundRead> read)
    {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(read));
        }
        wake_.notify_one();
    }

  private:
    BackgroundReader() : thread_([this] { run(); })
    {
    }
    ~BackgroundReader()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    void run()
    {
#if defined(__linux__) || defined(__ANDROID__)
        pthread_setname_np(pthread_self(), "storm-file-read");
#endif
        for (;;)
        {
            std::shared_ptr<BackgroundRead> read;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_)
                    return;
                read = std::move(queue_.front());
                queue_.pop_front();
            }
            // Nobody waits for it any more (the model went before its files came): not read.
            if (read.use_count() > 1)
                serve(*read);
            {
                std::lock_guard lock(g_doneMutex);
                read->done_.store(true, std::memory_order_release);
            }
            g_doneSignal.notify_all();
        }
    }

    static void serve(BackgroundRead &read)
    {
        read.found_ = read.candidates_.size();
        for (size_t i = 0; i < read.candidates_.size(); ++i)
        {
            std::ifstream file(std::filesystem::u8path(read.candidates_[i]), std::ios::binary | std::ios::ate);
            if (!file.is_open())
                continue;
            const std::streamoff size = file.tellg();
            auto bytes = std::make_shared<std::vector<char>>(size > 0 ? static_cast<size_t>(size) : 0);
            file.seekg(0);
            if (size < 0 || !file.read(bytes->data(), static_cast<std::streamsize>(bytes->size())))
            {
                // The game thread opens it itself and reports what it finds.
                spdlog::error("background read: {} opened but could not be read", read.candidates_[i]);
                read.error_ = true;
                return;
            }
            read.found_ = i;
            read.bytes_ = std::move(bytes);
            return;
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<BackgroundRead>> queue_;
    bool stop_ = false;
    std::thread thread_;
};

void BackgroundRead::wait() const
{
    std::unique_lock lock(g_doneMutex);
    g_doneSignal.wait(lock, [this] { return ready(); });
}

std::shared_ptr<BackgroundRead> ReadInBackground(std::vector<std::string> candidates)
{
    auto read = std::make_shared<BackgroundRead>(std::move(candidates));
    BackgroundReader::instance().push(read);
    return read;
}

namespace
{
std::unordered_map<std::string, Prefetched> &prefetched()
{
    static std::unordered_map<std::string, Prefetched> files;
    return files;
}
} // namespace

void PrefetchScope::add(const BackgroundRead &read)
{
    if (read.error_)
        return; // the readers open the files themselves
    auto &files = prefetched();
    const auto &candidates = read.candidates();
    for (size_t i = 0; i < candidates.size() && i <= read.found(); ++i)
    {
        Prefetched entry;
        entry.missing = i < read.found() || !read.bytes();
        if (!entry.missing)
            entry.bytes = read.bytes();
        if (files.insert_or_assign(candidates[i], std::move(entry)).second)
            paths_.push_back(candidates[i]);
    }
}

PrefetchScope::~PrefetchScope()
{
    auto &files = prefetched();
    for (const auto &path : paths_)
        files.erase(path);
}

const Prefetched *FindPrefetched(const std::string &resolved)
{
    const auto &files = prefetched();
    if (files.empty())
        return nullptr;
    const auto it = files.find(resolved);
    return it == files.end() ? nullptr : &it->second;
}

bool AnyPrefetched()
{
    return !prefetched().empty();
}
} // namespace storm::io
