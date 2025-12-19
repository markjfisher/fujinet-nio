#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace fujinet::core {

/**
 * Minimal synchronous pub/sub stream.
 *
 * Thread-safety:
 * - subscribe/unsubscribe protected by mutex
 * - publish takes a snapshot under mutex then calls callbacks outside the lock
 */
template <typename EventT>
class EventStream {
public:
    using Callback = std::function<void(const EventT&)>;

    struct Subscription {
        std::uint32_t id{0};
    };

    Subscription subscribe(Callback cb)
    {
        std::lock_guard<std::mutex> g(_mx);
        const std::uint32_t id = ++_nextId;
        _subs.push_back({id, std::move(cb)});
        return Subscription{id};
    }

    void unsubscribe(Subscription sub)
    {
        std::lock_guard<std::mutex> g(_mx);
        for (auto it = _subs.begin(); it != _subs.end(); ++it) {
            if (it->id == sub.id) {
                _subs.erase(it);
                return;
            }
        }
    }

    void publish(const EventT& ev)
    {
        std::vector<Subscriber> snap;
        {
            std::lock_guard<std::mutex> g(_mx);
            snap = _subs;
        }
        for (auto& s : snap) {
            if (s.cb) s.cb(ev);
        }
    }

private:
    struct Subscriber {
        std::uint32_t id;
        Callback cb;
    };

    std::mutex _mx;
    std::uint32_t _nextId{0};
    std::vector<Subscriber> _subs;
};

} // namespace fujinet::core
