#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fujinet::io::stream {

// Experimental stream transaction primitive. No wire command is allocated yet.
// One slot per stream, owned by the cooperative service loop (not thread safe).
// A successor request implicitly acknowledges the previous result. The caller
// must validate the session token before consulting this slot.
template<std::size_t RequestCapacity, std::size_t ResultCapacity, typename Sequence = std::uint16_t>
class ReplaySlot {
public:
    enum class Decision { Execute, Replay, Reject };

    Decision inspect(Sequence sequence, const std::uint8_t* bytes,
                     std::size_t size) const noexcept
    {
        if (size > RequestCapacity || (size && !bytes)) return Decision::Reject;
        if (!_valid) return sequence == 0 ? Decision::Execute : Decision::Reject;
        if (sequence == _sequence) {
            if (size != _requestSize) return Decision::Reject;
            for (std::size_t i = 0; i < size; ++i)
                if (_request[i] != bytes[i]) return Decision::Reject;
            return Decision::Replay;
        }
        // Never wrap a sequence within a session: delayed packets must not
        // acquire a new meaning after 65536 operations. Renew the session.
        return _sequence != std::numeric_limits<Sequence>::max() &&
               sequence == static_cast<Sequence>(_sequence + 1)
            ? Decision::Execute : Decision::Reject;
    }

    // Commit every terminal result, including would-block and errors. A caller
    // that has received would-block issues a NEW sequence to try again; a lost
    // would-block reply is replayed, not re-evaluated against changing state.
    // Validate result capacity BEFORE performing any irreversible operation.
    bool commit(Sequence sequence, const std::uint8_t* request,
                std::size_t requestSize, const std::uint8_t* result,
                std::size_t resultSize) noexcept
    {
        if (inspect(sequence, request, requestSize) != Decision::Execute ||
            resultSize > ResultCapacity || (resultSize && !result)) return false;
        for (std::size_t i = 0; i < requestSize; ++i) _request[i] = request[i];
        for (std::size_t i = 0; i < resultSize; ++i) _result[i] = result[i];
        _requestSize = requestSize;
        _resultSize = resultSize;
        _sequence = sequence;
        _valid = true;
        return true;
    }

    const std::uint8_t* result() const noexcept { return _result.data(); }
    std::size_t result_size() const noexcept { return _resultSize; }
    // Only after invalidating the old session token, never on a carrier retry.
    void reset() noexcept { _valid = false; _requestSize = _resultSize = 0; }

private:
    std::array<std::uint8_t, RequestCapacity> _request{};
    std::array<std::uint8_t, ResultCapacity> _result{};
    std::size_t _requestSize{0}, _resultSize{0};
    Sequence _sequence{0};
    bool _valid{false};
};

} // namespace fujinet::io::stream
