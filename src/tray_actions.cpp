#include "evox2/tray_actions.hpp"

#include <utility>

namespace evox2::tray {

bool WriteQuarantine::tripped() const noexcept
{
    return tripped_;
}

std::string_view WriteQuarantine::reason() const noexcept
{
    return reason_;
}

bool WriteQuarantine::trip(std::string reason)
{
    if (tripped_) {
        return false;
    }
    if (reason.empty()) {
        reason = "Unklarer P-MODE-Schreibausgang.";
    }
    reason_ = std::move(reason);
    tripped_ = true;
    return true;
}

bool ModeChangeGate::try_begin(const WriteQuarantine& quarantine) noexcept
{
    if (in_progress_ || quarantine.tripped()) {
        return false;
    }
    in_progress_ = true;
    return true;
}

bool ModeChangeGate::in_progress() const noexcept
{
    return in_progress_;
}

bool ModeChangeGate::write_permitted(const WriteQuarantine& quarantine) const noexcept
{
    return in_progress_ && !quarantine.tripped();
}

void ModeChangeGate::finish() noexcept
{
    in_progress_ = false;
}

GuardedModeChangeOutcome run_guarded_mode_change(
    ModeChangeGate& gate,
    const WriteQuarantine& quarantine,
    const std::function<bool()>& confirm,
    const std::function<void()>& write)
{
    if (!gate.try_begin(quarantine)) {
        return GuardedModeChangeOutcome::Rejected;
    }
    struct GateRelease final {
        ModeChangeGate& gate;
        ~GateRelease() { gate.finish(); }
    } gate_release {gate};

    if (!confirm()) {
        return GuardedModeChangeOutcome::Declined;
    }
    if (!gate.write_permitted(quarantine)) {
        return GuardedModeChangeOutcome::Blocked;
    }
    write();
    return GuardedModeChangeOutcome::Executed;
}

std::optional<PMode> target_mode(unsigned int command) noexcept
{
    switch (command) {
    case kCommandSetBalanced:
        return PMode::Balanced;
    case kCommandSetPerformance:
        return PMode::Performance;
    case kCommandSetQuiet:
        return PMode::Quiet;
    default:
        return std::nullopt;
    }
}

std::string confirmation_text(PMode current, PMode target)
{
    return "Firmware-P-MODE von " + std::string(mode_name(current)) + " auf "
        + std::string(mode_name(target)) + " umschalten?\n\n"
        + "Das Widget schreibt hoechstens einmal in den Embedded Controller und prueft danach "
          "den gelesenen Zustand. Bei unklarem Ergebnis erfolgt kein automatischer zweiter Schreibversuch.";
}

bool mode_switch_available(
    std::optional<PMode> observed_mode,
    bool exact_firmware,
    const WriteQuarantine& quarantine) noexcept
{
    return observed_mode.has_value() && exact_firmware && !quarantine.tripped();
}

std::string write_quarantine_detail(const WriteQuarantine& quarantine)
{
    if (!quarantine.tripped()) {
        return {};
    }
    return "P-MODE-Schreibausgang unklar: " + std::string(quarantine.reason())
        + "\n\nWeitere P-MODE-Schreibversuche sind fuer diesen Programmlauf gesperrt. "
          "Stelle den Hardwarezustand manuell stabil fest und starte das Widget erst danach neu.";
}

} // namespace evox2::tray
