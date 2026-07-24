// Part of the SkyRoads SDL port
//
// The reference design uses `Result<T, Error>`. In C++ we model the same failure
// modes with a single exception type. Loaders operate on trusted shipped
// assets, so throwing on malformed input keeps the call sites as clean as the
// `?`-propagation they mirror.
#pragma once

#include <stdexcept>
#include <string>

namespace skyroads::data {

// Mirrors the reference `Error` enum. `kind` records which variant threw so callers
// (and tests) can distinguish an expected end-of-stream from a real format
// error -- the compression decoder relies on this distinction.
class Error : public std::runtime_error {
public:
    enum class Kind {
        Io,
        InvalidFormat,
        UnexpectedEof,
    };

    Error(Kind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    Kind kind() const noexcept { return kind_; }

    static Error invalid_format(std::string message) {
        return Error(Kind::InvalidFormat, std::move(message));
    }

    static Error unexpected_eof(const char* context) {
        return Error(Kind::UnexpectedEof,
                     std::string("unexpected end of data: ") + context);
    }

    static Error io(std::string message) {
        return Error(Kind::Io, std::move(message));
    }

private:
    Kind kind_;
};

} // namespace skyroads::data
