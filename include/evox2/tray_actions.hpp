#pragma once

#include "evox2/core.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace evox2::tray {

inline constexpr unsigned int kCommandSetBalanced = 1101;
inline constexpr unsigned int kCommandSetPerformance = 1102;
inline constexpr unsigned int kCommandSetQuiet = 1103;

class WriteQuarantine final {
public:
    [[nodiscard]] bool tripped() const noexcept;
    [[nodiscard]] std::string_view reason() const noexcept;
    bool trip(std::string reason);

private:
    bool tripped_ = false;
    std::string reason_;
};

class ModeChangeGate final {
public:
    [[nodiscard]] bool try_begin(const WriteQuarantine& quarantine) noexcept;
    [[nodiscard]] bool in_progress() const noexcept;
    [[nodiscard]] bool write_permitted(const WriteQuarantine& quarantine) const noexcept;
    void finish() noexcept;

private:
    bool in_progress_ = false;
};

enum class GuardedModeChangeOutcome {
    Rejected,
    Declined,
    Blocked,
    Executed,
};

[[nodiscard]] GuardedModeChangeOutcome run_guarded_mode_change(
    ModeChangeGate& gate,
    const WriteQuarantine& quarantine,
    const std::function<bool()>& confirm,
    const std::function<void()>& write);

[[nodiscard]] std::optional<PMode> target_mode(unsigned int command) noexcept;
[[nodiscard]] std::string confirmation_text(PMode current, PMode target);
[[nodiscard]] bool mode_switch_available(
    std::optional<PMode> observed_mode,
    bool exact_firmware,
    const WriteQuarantine& quarantine) noexcept;
[[nodiscard]] std::string write_quarantine_detail(const WriteQuarantine& quarantine);

} // namespace evox2::tray
