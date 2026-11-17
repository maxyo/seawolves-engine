// Files read off the game thread for loads the game starts while it is played (a character walking in, a boarding
// fighter): the reader thread reads the bytes, the game thread builds from them when they are there.
//
//   * ReadInBackground: the reader thread reads the first of the candidate files that opens (resolved paths, the game
//     thread resolves them: FILE_SERVICE::ConvertPathResource is not thread-safe). Poll ready() or wait().
//   * PrefetchScope: for one synchronous build on the game thread, the readers that would open these files
//     (GEOM_SERVICE_R::OpenFile, Renderer::TextureLoad) take the bytes from here instead, and a candidate found
//     missing is not opened again. Game thread only; the entries go with the scope.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace storm::io
{
using FileBytes = std::shared_ptr<const std::vector<char>>;

class BackgroundRead
{
  public:
    explicit BackgroundRead(std::vector<std::string> candidates) : candidates_(std::move(candidates))
    {
    }
    bool ready() const
    {
        return done_.load(std::memory_order_acquire);
    }
    // Blocks until the reader thread has read (or found none of) the candidates.
    void wait() const;

    // Valid once ready(). The index of the candidate read, or candidates().size() when none opened.
    size_t found() const
    {
        return found_;
    }
    const std::vector<std::string> &candidates() const
    {
        return candidates_;
    }
    const FileBytes &bytes() const
    {
        return bytes_;
    }

  private:
    friend class BackgroundReader;
    friend class PrefetchScope;
    std::vector<std::string> candidates_;
    size_t found_ = 0;
    bool error_ = false; // one opened but could not be read (logged)
    FileBytes bytes_;
    std::atomic<bool> done_{false};
};

// Queued for the one reader thread (started on first use), read in the order asked.
std::shared_ptr<BackgroundRead> ReadInBackground(std::vector<std::string> candidates);

// What a reader finds for a resolved path inside a PrefetchScope.
struct Prefetched
{
    bool missing = false; // the background read found no such file
    FileBytes bytes;      // else its bytes
};

class PrefetchScope
{
  public:
    PrefetchScope() = default;
    ~PrefetchScope();
    PrefetchScope(const PrefetchScope &) = delete;
    PrefetchScope &operator=(const PrefetchScope &) = delete;

    // The candidates before the one read are missing; the one read has its bytes; those after it were not tried.
    void add(const BackgroundRead &read);

  private:
    std::vector<std::string> paths_;
};

// The entry of an open PrefetchScope for this resolved path, or nullptr. Game thread only.
const Prefetched *FindPrefetched(const std::string &resolved);
// Whether a PrefetchScope holds anything (a reader that would have to resolve its path first asks this).
bool AnyPrefetched();
} // namespace storm::io
