#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace evox2 {

enum class PMode : std::uint8_t {
    Balanced = 0x00,
    Performance = 0x01,
    Quiet = 0x02,
};

class EcProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class EcWriteOutcomeError : public EcProtocolError {
public:
    using EcProtocolError::EcProtocolError;
};

class UnsupportedHardwareError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

PMode decode_mode(std::uint8_t raw);
std::uint8_t encode_mode(PMode mode);
std::string_view mode_name(PMode mode);
std::string_view mode_name_lower(PMode mode);
bool looks_like_supported_board(const std::vector<std::string>& identity_values);

class IEcIo {
public:
    virtual ~IEcIo() = default;
    virtual std::uint8_t read_status() = 0;
    virtual void send_read_command() = 0;
    virtual void send_write_command() = 0;
    virtual void send_register_address(std::uint8_t address) = 0;
    virtual std::uint8_t read_data() = 0;
    virtual void send_data(std::uint8_t value) = 0;
};

class IPortIo {
public:
    virtual ~IPortIo() = default;
    virtual std::uint8_t read_port(std::uint8_t port) = 0;
    virtual void write_port(std::uint8_t port, std::uint8_t value) = 0;
};

class AcpiEcPortIo final : public IEcIo {
public:
    explicit AcpiEcPortIo(IPortIo& ports);
    std::uint8_t read_status() override;
    void send_read_command() override;
    void send_write_command() override;
    void send_register_address(std::uint8_t address) override;
    std::uint8_t read_data() override;
    void send_data(std::uint8_t value) override;

private:
    IPortIo& ports_;
};

class IEcRegisterReader {
public:
    virtual ~IEcRegisterReader() = default;
    virtual std::uint8_t read_byte(std::uint8_t address) = 0;
};

class AcpiEcRegisterReader final : public IEcRegisterReader {
public:
    explicit AcpiEcRegisterReader(IEcIo& io, unsigned int max_polls = 500);
    std::uint8_t read_byte(std::uint8_t address) override;

private:
    IEcIo& io_;
    unsigned int max_polls_;
    void wait_for_input_buffer_clear();
    void wait_for_output_buffer_empty();
    void wait_for_output_buffer_full();
};

class AcpiEcPModeWriter final {
public:
    explicit AcpiEcPModeWriter(IEcIo& io, unsigned int max_polls = 500);
    void write_mode(PMode mode);

private:
    IEcIo& io_;
    unsigned int max_polls_;
    void wait_for_input_buffer_clear();
    void wait_for_output_buffer_empty();
    void wait_until_write_ready();
};

struct Snapshot {
    PMode mode;
    std::uint8_t raw_mode;
    std::uint8_t firmware_major;
    std::uint8_t firmware_minor;

    [[nodiscard]] std::string firmware_version() const;
};

struct ModeTransitionResult {
    PMode authoritative_mode;
    bool changed;
};

class IPModeTransitionIo {
public:
    virtual ~IPModeTransitionIo() = default;
    virtual Snapshot read_snapshot() = 0;
    virtual void write_mode(PMode mode) = 0;
    virtual void wait_for_mode_settle() = 0;
    virtual PMode read_mode_once() = 0;
};

[[nodiscard]] ModeTransitionResult apply_mode_transition(
    IPModeTransitionIo& io,
    PMode expected_current,
    PMode target);

class EvoX2Probe final {
public:
    explicit EvoX2Probe(IEcRegisterReader& reader);
    [[nodiscard]] Snapshot read_snapshot();

private:
    IEcRegisterReader& reader_;
};

std::string format_text(const Snapshot& snapshot);
std::string format_json(const Snapshot& snapshot);

} // namespace evox2
