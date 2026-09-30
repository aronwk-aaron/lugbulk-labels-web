#include "rate_limits.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <sstream>

namespace lugbulk::limits {

namespace {

std::string lower_trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

RateLimiter::RateLimiter(double capacity, double per_second)
    : capacity_(capacity), per_second_(per_second), last_sweep_(std::chrono::steady_clock::now()) {}

std::optional<int> RateLimiter::take(const std::string& key) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mu_);
    sweep(now);
    // Bound memory: a flood of distinct keys (e.g. spoofed or rotating IPs)
    // can't grow the table without limit — past the cap, unknown keys are
    // refused until a sweep frees room.
    if (buckets_.size() >= kMaxKeys && !buckets_.count(key)) {
        last_sweep_ = {};  // sweep on the next call
        return 1;
    }
    auto [it, inserted] = buckets_.try_emplace(key, Bucket{capacity_, now});
    Bucket& b = it->second;
    double elapsed = std::chrono::duration<double>(now - b.updated).count();
    b.tokens = std::min(capacity_, b.tokens + elapsed * per_second_);
    b.updated = now;
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return std::nullopt;
    }
    return static_cast<int>(std::ceil((1.0 - b.tokens) / per_second_));
}

void RateLimiter::sweep(std::chrono::steady_clock::time_point now) {
    if (now - last_sweep_ < std::chrono::minutes(5)) return;
    last_sweep_ = now;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        double elapsed = std::chrono::duration<double>(now - it->second.updated).count();
        bool full = it->second.tokens + elapsed * per_second_ >= capacity_;
        it = full ? buckets_.erase(it) : std::next(it);
    }
}

JobGate::JobGate(int global_max) : global_max_(std::max(1, global_max)) {}

JobGate::Result JobGate::enter(int64_t user) {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_.count(user)) return {std::nullopt, Refusal::kUserBusy};
    if (static_cast<int>(running_.size()) >= global_max_) return {std::nullopt, Refusal::kServerBusy};
    running_.insert(user);
    Result r;
    r.ticket.emplace(this, user);
    return r;
}

void JobGate::leave(int64_t user) {
    std::lock_guard<std::mutex> lock(mu_);
    running_.erase(user);
}

JobGate::Ticket::~Ticket() {
    if (gate_) gate_->leave(user_);
}

Allowlist::Allowlist(const std::string& comma_separated) {
    std::stringstream ss(comma_separated);
    for (std::string item; std::getline(ss, item, ',');) {
        item = lower_trim(item);
        if (!item.empty()) entries_.push_back(item);
    }
}

bool Allowlist::allows(const std::string& email) const {
    if (entries_.empty()) return true;
    std::string e = lower_trim(email);
    size_t at = e.rfind('@');
    if (at == std::string::npos || at == 0) return false;
    std::string domain = e.substr(at);  // "@example.com"
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const std::string& entry) { return entry == e || entry == domain; });
}

}  // namespace lugbulk::limits
