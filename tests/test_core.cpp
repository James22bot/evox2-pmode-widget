#include "evox2/core.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

namespace {

using evox2::AcpiEcRegisterReader;
using evox2::EcProtocolError;
using evox2::EvoX2Probe;
using evox2::IEcIo;
using evox2::IEcRegisterReader;
using evox2::PMode;
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
    assert_equal(std::string_view("Performance"), evox2::mode_name(PMode::Performance), "mode label");
    throws<UnsupportedHardwareError>([] { (void)evox2::decode_mode(0x03); }, "unknown mode");
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
