#pragma once

#include "evox2/core.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace evox2::windows {

class DependencyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct BoardIdentity {
    std::vector<std::pair<std::string, std::string>> fields;
    bool supported;

    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] BoardIdentity read_board_identity();

class EcBackend final {
public:
    EcBackend();
    ~EcBackend();

    EcBackend(const EcBackend&) = delete;
    EcBackend& operator=(const EcBackend&) = delete;
    EcBackend(EcBackend&&) noexcept;
    EcBackend& operator=(EcBackend&&) noexcept;

    [[nodiscard]] Snapshot read_snapshot();
    [[nodiscard]] PMode read_mode();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace evox2::windows
