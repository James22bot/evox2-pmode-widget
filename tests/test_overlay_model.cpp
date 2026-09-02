#include "evox2/overlay_model.hpp"
#include "evox2/tray_actions.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using evox2::AcpiEcPortIo;
using evox2::IPortIo;
using evox2::PMode;
using evox2::overlay::Dimensions;
using evox2::overlay::OverlayModel;
using evox2::overlay::Position;
using evox2::overlay::Rectangle;
using evox2::overlay::Rgb;

int failures = 0;

class RecordingPortIo final : public IPortIo {
public:
    explicit RecordingPortIo(std::deque<std::uint8_t> reads)
        : reads_(std::move(reads))
    {
    }

    std::uint8_t read_port(std::uint8_t port) override
    {
        std::ostringstream operation;
        operation << "read:" << std::hex << static_cast<unsigned int>(port);
        operations.push_back(operation.str());
        if (reads_.empty()) {
            throw std::runtime_error("unexpected port read");
        }
        const auto value = reads_.front();
        reads_.pop_front();
        return value;
    }

    void write_port(std::uint8_t port, std::uint8_t value) override
    {
        std::ostringstream operation;
        operation << "write:" << std::hex << static_cast<unsigned int>(port)
                  << ':' << static_cast<unsigned int>(value);
        operations.push_back(operation.str());
    }

    std::vector<std::string> operations;

private:
    std::deque<std::uint8_t> reads_;
};

template <typename T>
void assert_equal(const T& expected, const T& actual, std::string_view label)
{
    if (!(expected == actual)) {
        std::ostringstream message;
        message << label << " mismatch";
        throw std::runtime_error(message.str());
    }
}

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_initial_state()
{
    OverlayModel model;
    check(!model.available(), "initial state must be unavailable");
    check(!model.observed_mode().has_value(), "initial state exposed a mode");
    assert_equal(std::string_view("P-MODE: --"), model.text(), "initial text");
    assert_equal(Rgb {150, 155, 165}, model.color(), "initial color");
    check(!model.detail().empty(), "initial detail missing");
}

void test_mode_colors_and_labels()
{
    OverlayModel model;

    check(model.set_mode(PMode::Quiet), "quiet transition not reported");
    assert_equal(PMode::Quiet, *model.observed_mode(), "quiet observed mode");
    assert_equal(std::string_view("P-MODE: Quiet"), model.text(), "quiet text");
    assert_equal(Rgb {80, 210, 120}, model.color(), "quiet color");
    assert_equal(std::string("EVO-X2 P-MODE: Quiet"), model.tray_tooltip(), "quiet tooltip");

    check(model.set_mode(PMode::Balanced), "balanced transition not reported");
    assert_equal(std::string_view("P-MODE: Balanced"), model.text(), "balanced text");
    assert_equal(Rgb {245, 180, 55}, model.color(), "balanced color");

    check(model.set_mode(PMode::Performance), "performance transition not reported");
    assert_equal(std::string_view("P-MODE: Performance"), model.text(), "performance text");
    assert_equal(Rgb {245, 85, 85}, model.color(), "performance color");
    check(!model.set_mode(PMode::Performance), "unchanged mode reported as changed");
}

void test_failure_never_leaves_stale_mode()
{
    OverlayModel model;
    model.set_mode(PMode::Performance);

    check(model.set_unavailable("EC busy"), "failure transition not reported");
    check(!model.available(), "failure still marked available");
    check(!model.observed_mode().has_value(), "failure retained observed mode");
    assert_equal(std::string_view("P-MODE: --"), model.text(), "failure text");
    assert_equal(Rgb {150, 155, 165}, model.color(), "failure color");
    assert_equal(std::string("EC busy"), model.detail(), "failure detail");
    assert_equal(std::string("EVO-X2 P-MODE: unavailable"), model.tray_tooltip(), "failure tooltip");

    check(model.set_unavailable("EC timeout"), "changed failure detail not reported");
    check(!model.set_unavailable("EC timeout"), "unchanged failure reported as changed");
}

void test_recovery()
{
    OverlayModel model;
    model.set_unavailable("EC busy");
    check(model.set_mode(PMode::Quiet), "recovery not reported");
    check(model.available(), "recovery not available");
    check(model.detail().empty(), "recovery retained error");
    assert_equal(std::string_view("P-MODE: Quiet"), model.text(), "recovery text");
}

void test_top_right_position()
{
    const Rectangle work_area {0, 0, 1920, 1040};
    const Dimensions size {210, 42};
    assert_equal(Position {1920 - 210 - 12, 12}, evox2::overlay::top_right_position(work_area, size, 12), "normal position");

    const Rectangle small_area {10, 20, 110, 80};
    const Dimensions oversized {200, 100};
    assert_equal(Position {22, 32}, evox2::overlay::top_right_position(small_area, oversized, 12), "clamped position");
}

void test_compact_widget_dimensions()
{
    assert_equal(132, evox2::overlay::kWidgetWidthLogicalPixels, "widget width");
    assert_equal(24, evox2::overlay::kWidgetHeightLogicalPixels, "widget height");
    assert_equal(6, evox2::overlay::kOverlayMarginLogicalPixels, "widget margin");
    assert_equal(10, evox2::overlay::kWidgetFontPoints, "widget font");
}

void test_acpi_read_port_mapping()
{
    RecordingPortIo ports({0x02, 0x01});
    AcpiEcPortIo io(ports);

    assert_equal<std::uint8_t>(0x02, io.read_status(), "status value");
    io.send_read_command();
    io.send_register_address(0x31);
    assert_equal<std::uint8_t>(0x01, io.read_data(), "data value");
    assert_equal(
        std::vector<std::string>({"read:66", "write:66:80", "write:62:31", "read:62"}),
        ports.operations,
        "ACPI EC read port sequence");
}

void test_acpi_write_port_mapping()
{
    RecordingPortIo ports({});
    AcpiEcPortIo io(ports);

    io.send_write_command();
    io.send_register_address(0x31);
    io.send_data(0x02);
    assert_equal(
        std::vector<std::string>({"write:66:81", "write:62:31", "write:62:2"}),
        ports.operations,
        "ACPI EC write port sequence");
}

void test_fixed_tray_mode_commands()
{
    assert_equal(
        PMode::Balanced,
        *evox2::tray::target_mode(evox2::tray::kCommandSetBalanced),
        "balanced tray command");
    assert_equal(
        PMode::Performance,
        *evox2::tray::target_mode(evox2::tray::kCommandSetPerformance),
        "performance tray command");
    assert_equal(
        PMode::Quiet,
        *evox2::tray::target_mode(evox2::tray::kCommandSetQuiet),
        "quiet tray command");
    check(!evox2::tray::target_mode(0xFFFFFFFFu).has_value(), "unknown tray command accepted");
}

void test_tray_registration_recovers_once_without_duplicate_adds()
{
    evox2::tray::IconRegistrationState registration;
    int attempts = 0;

    check(
        !registration.attempt_initial([&]() {
            ++attempts;
            return false;
        }),
        "failed initial tray registration reported available");
    check(!registration.available(), "failed initial tray registration stayed available");
    check(
        registration.recover_if_unavailable([&]() {
            ++attempts;
            return true;
        }),
        "tray registration did not recover after shell became available");
    check(registration.available(), "recovered tray registration stayed unavailable");
    check(
        !registration.recover_if_unavailable([&]() {
            ++attempts;
            return true;
        }),
        "available tray registration attempted a duplicate add");
    assert_equal(2, attempts, "tray registration attempts through recovery");
}

void test_mode_change_confirmation_is_explicit()
{
    const std::string prompt = evox2::tray::confirmation_text(PMode::Quiet, PMode::Performance);
    check(prompt.find("Quiet") != std::string::npos, "confirmation omitted current mode");
    check(prompt.find("Performance") != std::string::npos, "confirmation omitted target mode");
    check(prompt.find("hoechstens einmal") != std::string::npos,
          "confirmation overstated the at-most-once write contract");
    check(prompt.find("genau einmal") == std::string::npos,
          "confirmation still claims an exactly-once write");
    check(prompt.find("kein automatischer zweiter Schreibversuch") != std::string::npos,
          "confirmation omitted no-retry warning");
}

void test_write_quarantine_is_sticky_and_preserves_first_reason()
{
    evox2::tray::WriteQuarantine quarantine;

    check(!quarantine.tripped(), "write quarantine started tripped");
    check(quarantine.reason().empty(), "write quarantine started with a reason");
    check(quarantine.trip("first indeterminate outcome"), "first trip was not reported");
    check(quarantine.tripped(), "write quarantine did not trip");
    assert_equal(
        std::string_view("first indeterminate outcome"),
        quarantine.reason(),
        "write quarantine first reason");

    check(!quarantine.trip("later error"), "write quarantine accepted a second trip");
    assert_equal(
        std::string_view("first indeterminate outcome"),
        quarantine.reason(),
        "write quarantine preserved reason");
}

void test_mode_switch_gate_stays_closed_after_display_recovery()
{
    evox2::tray::WriteQuarantine quarantine;

    check(
        evox2::tray::mode_switch_available(PMode::Quiet, true, quarantine),
        "verified available mode did not permit switching");
    check(
        !evox2::tray::mode_switch_available(std::nullopt, true, quarantine),
        "missing observed mode permitted switching");
    check(
        !evox2::tray::mode_switch_available(PMode::Quiet, false, quarantine),
        "unverified firmware permitted switching");

    quarantine.trip("post-command readback failed");
    check(
        !evox2::tray::mode_switch_available(PMode::Balanced, true, quarantine),
        "recovered display mode reopened quarantined writes");
}

void test_mode_change_gate_rejects_reentrancy_and_quarantine_at_write_boundary()
{
    evox2::tray::WriteQuarantine quarantine;
    evox2::tray::ModeChangeGate gate;
    int writes = 0;
    evox2::tray::GuardedModeChangeOutcome nested =
        evox2::tray::GuardedModeChangeOutcome::Executed;

    const auto outer = evox2::tray::run_guarded_mode_change(
        gate,
        quarantine,
        [&]() {
            nested = evox2::tray::run_guarded_mode_change(
                gate,
                quarantine,
                []() { return true; },
                [&]() { ++writes; });
            quarantine.trip("nested indeterminate outcome");
            return true;
        },
        [&]() { ++writes; });

    assert_equal(
        evox2::tray::GuardedModeChangeOutcome::Rejected,
        nested,
        "nested mode-change outcome");
    assert_equal(
        evox2::tray::GuardedModeChangeOutcome::Blocked,
        outer,
        "outer mode-change outcome after quarantine");
    assert_equal(0, writes, "writes after nested request and quarantine");
    check(!gate.in_progress(), "mode-change gate stayed active after finish");
    check(!gate.try_begin(quarantine), "quarantined process reacquired the mode-change gate");

    evox2::tray::WriteQuarantine clean_quarantine;
    evox2::tray::ModeChangeGate clean_gate;
    const auto clean = evox2::tray::run_guarded_mode_change(
        clean_gate,
        clean_quarantine,
        []() { return true; },
        [&]() { ++writes; });
    assert_equal(
        evox2::tray::GuardedModeChangeOutcome::Executed,
        clean,
        "clean mode-change outcome");
    assert_equal(1, writes, "writes for clean mode change");
    check(!clean_gate.in_progress(), "clean mode-change gate stayed active after execution");
}

void test_write_quarantine_detail_preserves_anomaly_and_recovery_boundary()
{
    evox2::tray::WriteQuarantine quarantine;
    quarantine.trip("first hardware anomaly");

    const std::string detail = evox2::tray::write_quarantine_detail(quarantine);
    check(detail.find("first hardware anomaly") != std::string::npos,
          "quarantine detail omitted first anomaly");
    check(detail.find("Programmlauf gesperrt") != std::string::npos,
          "quarantine detail omitted process-lifetime boundary");
    check(detail.find("manuell") != std::string::npos,
          "quarantine detail omitted manual recovery requirement");
}

void run(std::string_view name, const std::function<void()>& test)
{
    try {
        test();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests {
        {"initial unavailable state", test_initial_state},
        {"mode labels and colors", test_mode_colors_and_labels},
        {"failure removes stale mode", test_failure_never_leaves_stale_mode},
        {"recovery from failure", test_recovery},
        {"top-right positioning", test_top_right_position},
        {"compact widget dimensions", test_compact_widget_dimensions},
        {"ACPI EC read-only port mapping", test_acpi_read_port_mapping},
        {"ACPI EC write port mapping", test_acpi_write_port_mapping},
        {"fixed tray mode commands", test_fixed_tray_mode_commands},
        {"tray registration recovers without duplicate adds", test_tray_registration_recovers_once_without_duplicate_adds},
        {"explicit mode-change confirmation", test_mode_change_confirmation_is_explicit},
        {"sticky write quarantine", test_write_quarantine_is_sticky_and_preserves_first_reason},
        {"write quarantine survives display recovery", test_mode_switch_gate_stays_closed_after_display_recovery},
        {"mode-change reentrancy gate", test_mode_change_gate_rejects_reentrancy_and_quarantine_at_write_boundary},
        {"write quarantine operator detail", test_write_quarantine_detail_preserves_anomaly_and_recovery_boundary},
    };

    for (const auto& [name, test] : tests) {
        run(name, test);
    }

    std::cout << (tests.size() - static_cast<std::size_t>(failures)) << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
