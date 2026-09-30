// Abuse protection: rate limits, concurrency caps and sign-in allowlist.
//
// Generating a report is the expensive thing this server does (a Google
// Sheets fetch, dozens of part-photo downloads and BrickLink lookups, PDF
// rendering), so one person hammering the buttons — or a script — must
// not be able to tie up every worker thread or burn the shared BrickLink
// quota. See main.cpp for where each limit applies.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lugbulk::limits {

// Token bucket per key (user id or client IP): `capacity` requests at
// once, refilled at `per_second`. Thread-safe.
class RateLimiter {
public:
    RateLimiter(double capacity, double per_second);

    // Takes a token for `key`. On refusal returns the seconds until one is
    // available (for a Retry-After header).
    std::optional<int> take(const std::string& key);

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point updated;
    };
    void sweep(std::chrono::steady_clock::time_point now);  // drop full, idle buckets

    const double capacity_, per_second_;
    std::mutex mu_;
    std::map<std::string, Bucket> buckets_;
    std::chrono::steady_clock::time_point last_sweep_;
};

// Caps concurrent heavy jobs: at most `global_max` at a time server-wide,
// and one per user. Callers hold the returned Ticket for the job's
// duration; a refused request gets nullopt and should answer 429/503
// immediately rather than queue (queueing would still pin a worker thread).
class JobGate {
public:
    explicit JobGate(int global_max);

    class Ticket {
    public:
        Ticket(JobGate* gate, int64_t user) : gate_(gate), user_(user) {}
        Ticket(Ticket&& other) noexcept : gate_(other.gate_), user_(other.user_) { other.gate_ = nullptr; }
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket& operator=(Ticket&&) = delete;
        ~Ticket();

    private:
        JobGate* gate_;
        int64_t user_;
    };

    enum class Refusal { kUserBusy, kServerBusy };
    // A ticket, or why not.
    struct Result {
        std::optional<Ticket> ticket;
        Refusal refusal = Refusal::kServerBusy;
    };
    Result enter(int64_t user);

private:
    void leave(int64_t user);

    const int global_max_;
    std::mutex mu_;
    std::set<int64_t> running_;
};

// A daily allowance shared by the whole server (UTC days), e.g. BrickLink
// API calls. Thread-safe.
class DailyBudget {
public:
    explicit DailyBudget(int per_day);
    // Takes up to `want` units; returns how many were granted.
    int take(int want);

private:
    const int per_day_;
    std::mutex mu_;
    int64_t day_ = -1;
    int used_ = 0;
};

// Who may sign in. Entries are full addresses ("ann@example.com") or
// domains ("@example.com"), case-insensitive. An empty allowlist lets any
// Google account in (Google's own "Testing" mode test-user list is then
// the only gate).
class Allowlist {
public:
    explicit Allowlist(const std::string& comma_separated);
    bool allows(const std::string& email) const;
    bool empty() const { return entries_.empty(); }

private:
    std::vector<std::string> entries_;
};

}  // namespace lugbulk::limits
