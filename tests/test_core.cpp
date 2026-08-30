#include "evox2/core.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

namespace {

using evox2::AcpiEcPModeWriter;
using evox2::AcpiEcRegisterReader;
using evox2::EcProtocolError;
using evox2::EcWriteOutcomeError;
using evox2::EvoX2Probe;
using evox2::IEcIo;
using evox2::IEcRegisterReader;
using evox2::IPModeTransitionIo;
using evox2::ModeTransitionResult;
using evox2::PMode;
using evox2::Snapshot;
using evox2::UnsupportedHardwareError;

int failures = 0;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename T>
void assert_equal(const T& expected, const T& actual, std::string_view label)
{
    if (!(expected == actual)) {
        std::ostringstream message;
        message << label << " mismatch";
        throw std::runtime_error(message.str());
    }
}

template <typename Exception, typename Callable>
void throws(Callable&& callable, std::string_view label)
{
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string(label) + " did not throw");
}

class ScriptedEcIo final : public IEcIo {
public:
    ScriptedEcIo(std::deque<std::uint8_t> statuses, std::deque<std::uint8_t> data)
        : statuses_(std::move(statuses)), data_(std::move(data))
    {
    }

    std::uint8_t read_status() override
    {
        operations.emplace_back("status");
        if (statuses_.empty()) {
            throw std::runtime_error("status script exhausted");
        }
        const auto value = statuses_.front();
        statuses_.pop_front();
        return value;
    }

    void send_read_command() override { operations.emplace_back("read-command"); }

    void send_write_command() override { operations.emplace_back("write-command"); }

    void send_register_address(std::uint8_t address) override
    {
        std::ostringstream item;
        item << "address:" << std::hex << std::uppercase;
        item.width(2);
        item.fill('0');
        item << static_cast<unsigned int>(address);
        operations.push_back(item.str());
    }

    std::uint8_t read_data() override
    {
        operations.emplace_back("data");
        if (data_.empty()) {
            throw std::runtime_error("data script exhausted");
        }
        const auto value = data_.front();
        data_.pop_front();
        return value;
    }

    void send_data(std::uint8_t value) override
    {
        std::ostringstream item;
        item << "value:" << std::hex << std::uppercase;
        item.width(2);
        item.fill('0');
        item << static_cast<unsigned int>(value);
        operations.push_back(item.str());
    }

    std::vector<std::string> operations;

private:
    std::deque<std::uint8_t> statuses_;
    std::deque<std::uint8_t> data_;
};

class SequenceRegisterReader final : public IEcRegisterReader {
public:
    explicit SequenceRegisterReader(std::map<std::uint8_t, std::deque<std::uint8_t>> values)
        : values_(std::move(values))
    {
    }

    std::uint8_t read_byte(std::uint8_t address) override
    {
        auto& values = values_.at(address);
        if (values.empty()) {
            throw std::runtime_error("register script exhausted");
        }
        const auto value = values.front();
        values.pop_front();
        return value;
    }

private:
    std::map<std::uint8_t, std::deque<std::uint8_t>> values_;
};

class ScriptedTransitionIo final : public IPModeTransitionIo {
public:
    ScriptedTransitionIo(Snapshot snapshot, std::deque<PMode> readback)
        : snapshot_(snapshot), readback_(std::move(readback))
    {
    }

    Snapshot read_snapshot() override
    {
        operations.emplace_back("snapshot");
        return snapshot_;
    }

    void write_mode(PMode mode) override
    {
        operations.emplace_back("write:" + std::string(evox2::mode_name(mode)));
        ++writes;
        if (write_error) {
            throw EcProtocolError("scripted write failure");
        }
    }

    void wait_for_mode_settle() override
    {
        operations.emplace_back("wait");
        ++waits;
        if (wait_error) {
            throw EcProtocolError("scripted settle failure");
        }
    }

    PMode read_mode_once() override
    {
        operations.emplace_back("readback");
        if (readback_.empty()) {
            throw EcProtocolError("readback script exhausted");
        }
        const PMode mode = readback_.front();
        readback_.pop_front();
        return mode;
    }

    std::vector<std::string> operations;
    unsigned int writes = 0;
    unsigned int waits = 0;
    bool write_error = false;
    bool wait_error = false;

private:
    Snapshot snapshot_;
    std::deque<PMode> readback_;
};

Snapshot transition_snapshot(PMode mode, std::uint8_t firmware_minor = 0x08)
{
    return Snapshot {
        .mode = mode,
        .raw_mode = evox2::encode_mode(mode),
        .firmware_major = 0x01,
        .firmware_minor = firmware_minor,
        .ec_temperature_celsius = 45,
    };
}

std::map<std::uint8_t, std::deque<std::uint8_t>> valid_registers()
{
    return {
        {0x31, {0x00, 0x00}},
        {0x00, {0x01}},
        {0x01, {0x09}},
        {0x70, {42}},
    };
}

void test_mode_mapping()
{
    assert_equal(PMode::Balanced, evox2::decode_mode(0x00), "balanced");
    assert_equal(PMode::Performance, evox2::decode_mode(0x01), "performance");
    assert_equal(PMode::Quiet, evox2::decode_mode(0x02), "quiet");
    assert_equal<std::uint8_t>(0x00, evox2::encode_mode(PMode::Balanced), "balanced encoding");
    assert_equal<std::uint8_t>(0x01, evox2::encode_mode(PMode::Performance), "performance encoding");
    assert_equal<std::uint8_t>(0x02, evox2::encode_mode(PMode::Quiet), "quiet encoding");
    assert_equal(std::string_view("Performance"), evox2::mode_name(PMode::Performance), "mode label");
    throws<UnsupportedHardwareError>([] { (void)evox2::decode_mode(0x03); }, "unknown mode");
    throws<UnsupportedHardwareError>(
        [] { (void)evox2::encode_mode(static_cast<PMode>(0x03)); },
        "unknown mode encoding");
}

void test_board_identity()
{
    check(evox2::looks_like_supported_board({"GMKtec", "EVO-X2"}), "EVO-X2 not recognized");
    check(evox2::looks_like_supported_board({"Sixunited", "SU_AXB35-02"}), "AXB35 not recognized");
    check(evox2::looks_like_supported_board({"EVO X2"}), "spaced EVO X2 not recognized");
    check(!evox2::looks_like_supported_board({"Unrelated", "Desktop"}), "unrelated board accepted");
}

void test_read_transaction()
{
    ScriptedEcIo io({0x00, 0x00, 0x00, 0x01}, {0x01});
    AcpiEcRegisterReader reader(io, 4);
    assert_equal<std::uint8_t>(0x01, reader.read_byte(0x31), "register value");
    assert_equal(
        std::vector<std::string>({"status", "status", "read-command", "status", "address:31", "status", "data"}),
        io.operations,
        "read transaction");
}

void test_busy_timeout()
{
    ScriptedEcIo io({0x02, 0x02, 0x02}, {});
    AcpiEcRegisterReader reader(io, 3);
    throws<EcProtocolError>([&] { (void)reader.read_byte(0x31); }, "input timeout");
    assert_equal(std::vector<std::string>({"status", "status", "status"}), io.operations, "timeout trace");
}

void test_pending_output_fails_closed()
{
    ScriptedEcIo io({0x00, 0x01, 0x01, 0x01}, {});
    AcpiEcRegisterReader reader(io, 3);
    throws<EcProtocolError>([&] { (void)reader.read_byte(0x31); }, "pending output");
    assert_equal(
        std::vector<std::string>({"status", "status", "status", "status"}),
        io.operations,
        "pending-output trace");
}

void test_write_transaction()
{
    ScriptedEcIo io({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {});
    AcpiEcPModeWriter writer(io, 4);
    writer.write_mode(PMode::Performance);
    assert_equal(
        std::vector<std::string>({
            "status",
            "status",
            "write-command",
            "status",
            "status",
            "address:31",
            "status",
            "status",
            "value:01",
            "status",
            "status",
        }),
        io.operations,
        "write transaction");
}

void test_write_preflight_pending_output_emits_no_command()
{
    ScriptedEcIo io({0x00, 0x01, 0x01, 0x01}, {});
    AcpiEcPModeWriter writer(io, 3);
    throws<EcProtocolError>([&] { writer.write_mode(PMode::Quiet); }, "write pending output");
    assert_equal(
        std::vector<std::string>({"status", "status", "status", "status"}),
        io.operations,
        "write preflight trace");
}

void test_write_failure_after_command_is_indeterminate_and_not_retried()
{
    ScriptedEcIo io({0x00, 0x00, 0x02, 0x02, 0x02}, {});
    AcpiEcPModeWriter writer(io, 3);
    throws<EcWriteOutcomeError>(
        [&] { writer.write_mode(PMode::Balanced); },
        "write outcome after command");
    assert_equal(
        std::vector<std::string>({"status", "status", "write-command", "status", "status", "status"}),
        io.operations,
        "post-command failure trace");
}

void test_write_failure_after_data_is_indeterminate_and_not_retried()
{
    ScriptedEcIo io({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01}, {});
    AcpiEcPModeWriter writer(io, 3);
    throws<EcWriteOutcomeError>(
        [&] { writer.write_mode(PMode::Quiet); },
        "write outcome after data");
    assert_equal(
        std::vector<std::string>({
            "status",
            "status",
            "write-command",
            "status",
            "status",
            "address:31",
            "status",
            "status",
            "value:02",
            "status",
            "status",
            "status",
            "status",
        }),
        io.operations,
        "post-data failure trace");
}

void test_invalid_write_target_emits_no_port_operation()
{
    ScriptedEcIo io({}, {});
    AcpiEcPModeWriter writer(io, 3);
    throws<UnsupportedHardwareError>(
        [&] { writer.write_mode(static_cast<PMode>(0x03)); },
        "invalid write target");
    check(io.operations.empty(), "invalid target touched EC ports");
}

void test_verified_mode_transition_applies_after_stable_readback()
{
    ScriptedTransitionIo io(
        transition_snapshot(PMode::Quiet),
        {PMode::Performance, PMode::Performance});
    const ModeTransitionResult result = evox2::apply_mode_transition(
        io,
        PMode::Quiet,
        PMode::Performance);

    check(result.changed, "applied transition not marked changed");
    assert_equal(PMode::Performance, result.authoritative_mode, "applied authoritative mode");
    assert_equal<unsigned int>(1, io.writes, "applied write count");
    assert_equal<unsigned int>(1, io.waits, "applied settle count");
    assert_equal(
        std::vector<std::string>({"snapshot", "write:Performance", "wait", "readback", "readback"}),
        io.operations,
        "applied transition trace");
}

void test_verified_mode_transition_noop_emits_no_write()
{
    ScriptedTransitionIo io(transition_snapshot(PMode::Balanced), {});
    const ModeTransitionResult result = evox2::apply_mode_transition(
        io,
        PMode::Balanced,
        PMode::Balanced);

    check(!result.changed, "no-op transition marked changed");
    assert_equal(PMode::Balanced, result.authoritative_mode, "no-op authoritative mode");
    assert_equal<unsigned int>(0, io.writes, "no-op write count");
    assert_equal(std::vector<std::string>({"snapshot"}), io.operations, "no-op transition trace");
}

void test_verified_mode_transition_accepts_race_already_at_target_without_write()
{
    ScriptedTransitionIo io(transition_snapshot(PMode::Performance), {});
    const ModeTransitionResult result = evox2::apply_mode_transition(
        io,
        PMode::Quiet,
        PMode::Performance);

    check(!result.changed, "race-at-target transition marked changed");
    assert_equal(PMode::Performance, result.authoritative_mode, "race-at-target authoritative mode");
    assert_equal<unsigned int>(0, io.writes, "race-at-target write count");
    assert_equal(std::vector<std::string>({"snapshot"}), io.operations, "race-at-target trace");
}

void test_verified_mode_transition_rejects_stale_confirmation()
{
    ScriptedTransitionIo io(transition_snapshot(PMode::Balanced), {});
    throws<EcProtocolError>(
        [&] { (void)evox2::apply_mode_transition(io, PMode::Quiet, PMode::Performance); },
        "stale confirmation");
    assert_equal<unsigned int>(0, io.writes, "stale confirmation write count");
    assert_equal(std::vector<std::string>({"snapshot"}), io.operations, "stale confirmation trace");
}

void test_verified_mode_transition_requires_exact_firmware()
{
    ScriptedTransitionIo io(transition_snapshot(PMode::Quiet, 0x09), {});
    throws<UnsupportedHardwareError>(
        [&] { (void)evox2::apply_mode_transition(io, PMode::Quiet, PMode::Performance); },
        "unsupported write firmware");
    assert_equal<unsigned int>(0, io.writes, "unsupported firmware write count");
    assert_equal(std::vector<std::string>({"snapshot"}), io.operations, "unsupported firmware trace");
}

void test_verified_mode_transition_rejects_invalid_target_before_io()
{
    ScriptedTransitionIo io(transition_snapshot(PMode::Quiet), {});
    throws<UnsupportedHardwareError>(
        [&] {
            (void)evox2::apply_mode_transition(
                io,
                PMode::Quiet,
                static_cast<PMode>(0x03));
        },
        "invalid transition target");
    check(io.operations.empty(), "invalid transition target touched hardware boundary");
}

void test_verified_mode_transition_mismatched_readback_is_indeterminate()
{
    ScriptedTransitionIo io(
        transition_snapshot(PMode::Quiet),
        {PMode::Balanced, PMode::Balanced});
    throws<EcWriteOutcomeError>(
        [&] { (void)evox2::apply_mode_transition(io, PMode::Quiet, PMode::Performance); },
        "mismatched readback");
    assert_equal<unsigned int>(1, io.writes, "mismatched readback write count");
    assert_equal<unsigned int>(1, io.waits, "mismatched readback settle count");
}

void test_verified_mode_transition_changing_readback_is_indeterminate()
{
    ScriptedTransitionIo io(
        transition_snapshot(PMode::Quiet),
        {PMode::Performance, PMode::Balanced});
    throws<EcWriteOutcomeError>(
        [&] { (void)evox2::apply_mode_transition(io, PMode::Quiet, PMode::Performance); },
        "changing readback");
    assert_equal<unsigned int>(1, io.writes, "changing readback write count");
}

void test_verified_mode_transition_settle_failure_is_indeterminate()
{
    ScriptedTransitionIo io(transition_snapshot(PMode::Quiet), {});
    io.wait_error = true;
    throws<EcWriteOutcomeError>(
        [&] { (void)evox2::apply_mode_transition(io, PMode::Quiet, PMode::Performance); },
        "settle failure");
    assert_equal<unsigned int>(1, io.writes, "settle failure write count");
    assert_equal<unsigned int>(1, io.waits, "settle failure wait count");
}

void test_snapshot_and_formatting()
{
    auto values = valid_registers();
    values[0x31] = {0x01, 0x01};
    values[0x70] = {47};
    SequenceRegisterReader reader(std::move(values));
    const auto snapshot = EvoX2Probe(reader).read_snapshot();

    assert_equal(PMode::Performance, snapshot.mode, "snapshot mode");
    assert_equal<std::uint8_t>(0x01, snapshot.raw_mode, "snapshot raw mode");
    assert_equal(std::string("1.09"), snapshot.firmware_version(), "firmware version");
    assert_equal<std::uint8_t>(47, snapshot.ec_temperature_celsius, "temperature");
    assert_equal(
        std::string("P-MODE: Performance\nRaw: 0x01\nEC firmware: 1.09\nEC temperature: 47 C"),
        evox2::format_text(snapshot),
        "text output");
    assert_equal(
        std::string("{\"mode\":\"performance\",\"raw\":1,\"ec_firmware\":\"1.09\",\"ec_temperature_c\":47}"),
        evox2::format_json(snapshot),
        "JSON output");
}

void test_invalid_firmware()
{
    auto values = valid_registers();
    values[0x00] = {0x00};
    values[0x01] = {0x00};
    SequenceRegisterReader reader(std::move(values));
    throws<UnsupportedHardwareError>([&] { (void)EvoX2Probe(reader).read_snapshot(); }, "invalid firmware");
}

void test_invalid_temperature()
{
    auto values = valid_registers();
    values[0x70] = {0xFF};
    SequenceRegisterReader reader(std::move(values));
    throws<UnsupportedHardwareError>([&] { (void)EvoX2Probe(reader).read_snapshot(); }, "invalid temperature");
}

void test_changed_mode()
{
    auto values = valid_registers();
    values[0x31] = {0x00, 0x01};
    SequenceRegisterReader reader(std::move(values));
    throws<EcProtocolError>([&] { (void)EvoX2Probe(reader).read_snapshot(); }, "changed mode");
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
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests = {
        {"documented P-MODE map", test_mode_mapping},
        {"board identity gate", test_board_identity},
        {"read-only ACPI EC transaction", test_read_transaction},
        {"EC timeout fails closed", test_busy_timeout},
        {"pending EC output fails closed", test_pending_output_fails_closed},
        {"fixed P-MODE write transaction", test_write_transaction},
        {"write preflight pending output emits no command", test_write_preflight_pending_output_emits_no_command},
        {"post-command failure is indeterminate", test_write_failure_after_command_is_indeterminate_and_not_retried},
        {"post-data failure is indeterminate", test_write_failure_after_data_is_indeterminate_and_not_retried},
        {"invalid write target emits no port operation", test_invalid_write_target_emits_no_port_operation},
        {"verified mode transition applies", test_verified_mode_transition_applies_after_stable_readback},
        {"verified mode transition no-op", test_verified_mode_transition_noop_emits_no_write},
        {"verified mode transition accepts race already at target", test_verified_mode_transition_accepts_race_already_at_target_without_write},
        {"verified mode transition rejects stale confirmation", test_verified_mode_transition_rejects_stale_confirmation},
        {"verified mode transition requires exact firmware", test_verified_mode_transition_requires_exact_firmware},
        {"verified mode transition rejects invalid target", test_verified_mode_transition_rejects_invalid_target_before_io},
        {"verified mode transition rejects mismatched readback", test_verified_mode_transition_mismatched_readback_is_indeterminate},
        {"verified mode transition rejects changing readback", test_verified_mode_transition_changing_readback_is_indeterminate},
        {"verified mode transition reports settle failure", test_verified_mode_transition_settle_failure_is_indeterminate},
        {"snapshot validation and output", test_snapshot_and_formatting},
        {"invalid firmware fails closed", test_invalid_firmware},
        {"invalid temperature fails closed", test_invalid_temperature},
        {"concurrent mode change fails closed", test_changed_mode},
    };
    for (const auto& [name, test] : tests) {
        run(name, test);
    }

    std::cout << (tests.size() - failures) << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
