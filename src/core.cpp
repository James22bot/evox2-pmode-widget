#include "evox2/core.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace evox2 {
namespace {

constexpr std::uint8_t kInputBufferFull = 0x02;
constexpr std::uint8_t kOutputBufferFull = 0x01;
constexpr std::uint8_t kEcCommandPort = 0x66;
constexpr std::uint8_t kEcDataPort = 0x62;
constexpr std::uint8_t kEcReadCommand = 0x80;
constexpr std::uint8_t kEcWriteCommand = 0x81;
constexpr std::uint8_t kPModeRegister = 0x31;
constexpr std::uint8_t kFirmwareMajorRegister = 0x00;
constexpr std::uint8_t kFirmwareMinorRegister = 0x01;
constexpr std::uint8_t kTemperatureRegister = 0x70;

std::string normalized_identity(std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return normalized;
}

std::string hexadecimal_byte(std::uint8_t value)
{
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned int>(value);
    return output.str();
}

} // namespace

PMode decode_mode(std::uint8_t raw)
{
    switch (raw) {
    case 0x00:
        return PMode::Balanced;
    case 0x01:
        return PMode::Performance;
    case 0x02:
        return PMode::Quiet;
    default:
        throw UnsupportedHardwareError("Unbekannter P-MODE-Rohwert " + hexadecimal_byte(raw) + ".");
    }
}

std::uint8_t encode_mode(PMode mode)
{
    switch (mode) {
    case PMode::Balanced:
        return 0x00;
    case PMode::Performance:
        return 0x01;
    case PMode::Quiet:
        return 0x02;
    }
    throw UnsupportedHardwareError("Ungueltiger interner P-MODE-Wert.");
}

std::string_view mode_name(PMode mode)
{
    switch (mode) {
    case PMode::Balanced:
        return "Balanced";
    case PMode::Performance:
        return "Performance";
    case PMode::Quiet:
        return "Quiet";
    }
    throw UnsupportedHardwareError("Ungueltiger interner P-MODE-Wert.");
}

std::string_view mode_name_lower(PMode mode)
{
    switch (mode) {
    case PMode::Balanced:
        return "balanced";
    case PMode::Performance:
        return "performance";
    case PMode::Quiet:
        return "quiet";
    }
    throw UnsupportedHardwareError("Ungueltiger interner P-MODE-Wert.");
}

bool looks_like_supported_board(const std::vector<std::string>& identity_values)
{
    return std::any_of(identity_values.begin(), identity_values.end(), [](const std::string& value) {
        const std::string normalized = normalized_identity(value);
        return normalized.find("EVOX2") != std::string::npos || normalized.find("AXB35") != std::string::npos;
    });
}

AcpiEcPortIo::AcpiEcPortIo(IPortIo& ports)
    : ports_(ports)
{
}

std::uint8_t AcpiEcPortIo::read_status()
{
    return ports_.read_port(kEcCommandPort);
}

void AcpiEcPortIo::send_read_command()
{
    ports_.write_port(kEcCommandPort, kEcReadCommand);
}

void AcpiEcPortIo::send_write_command()
{
    ports_.write_port(kEcCommandPort, kEcWriteCommand);
}

void AcpiEcPortIo::send_register_address(std::uint8_t address)
{
    ports_.write_port(kEcDataPort, address);
}

std::uint8_t AcpiEcPortIo::read_data()
{
    return ports_.read_port(kEcDataPort);
}

void AcpiEcPortIo::send_data(std::uint8_t value)
{
    ports_.write_port(kEcDataPort, value);
}

AcpiEcRegisterReader::AcpiEcRegisterReader(IEcIo& io, unsigned int max_polls)
    : io_(io), max_polls_(max_polls)
{
    if (max_polls_ == 0) {
        throw std::invalid_argument("max_polls muss groesser als null sein.");
    }
}

void AcpiEcRegisterReader::wait_for_input_buffer_clear()
{
    for (unsigned int attempt = 0; attempt < max_polls_; ++attempt) {
        if ((io_.read_status() & kInputBufferFull) == 0) {
            return;
        }
    }
    throw EcProtocolError("EC-Zeitueberschreitung: Eingabepuffer blieb belegt.");
}

void AcpiEcRegisterReader::wait_for_output_buffer_full()
{
    for (unsigned int attempt = 0; attempt < max_polls_; ++attempt) {
        if ((io_.read_status() & kOutputBufferFull) != 0) {
            return;
        }
    }
    throw EcProtocolError("EC-Zeitueberschreitung: Keine Lesedaten bereitgestellt.");
}

void AcpiEcRegisterReader::wait_for_output_buffer_empty()
{
    for (unsigned int attempt = 0; attempt < max_polls_; ++attempt) {
        if ((io_.read_status() & kOutputBufferFull) == 0) {
            return;
        }
    }
    throw EcProtocolError(
        "EC-Ausgabepuffer enthaelt bereits fremde Daten; ohne Konsumieren abgebrochen.");
}

std::uint8_t AcpiEcRegisterReader::read_byte(std::uint8_t address)
{
    wait_for_input_buffer_clear();
    wait_for_output_buffer_empty();
    io_.send_read_command();
    wait_for_input_buffer_clear();
    io_.send_register_address(address);
    wait_for_output_buffer_full();
    return io_.read_data();
}

AcpiEcPModeWriter::AcpiEcPModeWriter(IEcIo& io, unsigned int max_polls)
    : io_(io), max_polls_(max_polls)
{
    if (max_polls_ == 0) {
        throw std::invalid_argument("max_polls muss groesser als null sein.");
    }
}

void AcpiEcPModeWriter::wait_for_input_buffer_clear()
{
    for (unsigned int attempt = 0; attempt < max_polls_; ++attempt) {
        if ((io_.read_status() & kInputBufferFull) == 0) {
            return;
        }
    }
    throw EcProtocolError("EC-Zeitueberschreitung: Eingabepuffer blieb belegt.");
}

void AcpiEcPModeWriter::wait_for_output_buffer_empty()
{
    for (unsigned int attempt = 0; attempt < max_polls_; ++attempt) {
        if ((io_.read_status() & kOutputBufferFull) == 0) {
            return;
        }
    }
    throw EcProtocolError(
        "EC-Ausgabepuffer enthaelt bereits fremde Daten; P-MODE-Schreiben abgebrochen.");
}

void AcpiEcPModeWriter::wait_until_write_ready()
{
    wait_for_input_buffer_clear();
    wait_for_output_buffer_empty();
}

void AcpiEcPModeWriter::write_mode(PMode mode)
{
    const std::uint8_t value = encode_mode(mode);
    wait_until_write_ready();

    try {
        io_.send_write_command();
        wait_until_write_ready();
        io_.send_register_address(kPModeRegister);
        wait_until_write_ready();
        io_.send_data(value);
        wait_until_write_ready();
    } catch (const EcWriteOutcomeError&) {
        throw;
    } catch (const std::exception& error) {
        throw EcWriteOutcomeError(
            "P-MODE-Schreibbefehl wurde begonnen; Hardwarezustand ist unklar: "
            + std::string(error.what()));
    }
}

std::string Snapshot::firmware_version() const
{
    std::ostringstream output;
    output << static_cast<unsigned int>(firmware_major) << '.' << std::setw(2) << std::setfill('0')
           << static_cast<unsigned int>(firmware_minor);
    return output.str();
}

ModeTransitionResult apply_mode_transition(
    IPModeTransitionIo& io,
    PMode expected_current,
    PMode target)
{
    (void)encode_mode(expected_current);
    (void)encode_mode(target);

    const Snapshot before = io.read_snapshot();
    if (before.raw_mode != encode_mode(before.mode)) {
        throw UnsupportedHardwareError("P-MODE-Snapshot ist intern widerspruechlich; Schreiben verweigert.");
    }
    if (before.firmware_major != 0x01 || before.firmware_minor != 0x08) {
        throw UnsupportedHardwareError(
            "P-MODE-Schreiben ist nur fuer die hardwaregepruefte EC-Firmware 1.08 freigegeben.");
    }
    if (before.mode == target) {
        return ModeTransitionResult {.authoritative_mode = before.mode, .changed = false};
    }
    if (before.mode != expected_current) {
        throw EcProtocolError(
            "P-MODE hat sich seit der Bestaetigung geaendert; bitte aktuellen Zustand erneut pruefen.");
    }

    io.write_mode(target);
    try {
        io.wait_for_mode_settle();
        const PMode first = io.read_mode_once();
        const PMode second = io.read_mode_once();
        (void)encode_mode(first);
        (void)encode_mode(second);
        if (first != second) {
            throw EcWriteOutcomeError(
                "P-MODE-Readback war nicht stabil; Hardwarezustand ist unklar.");
        }
        if (first != target) {
            throw EcWriteOutcomeError(
                "P-MODE-Readback entspricht nicht dem Ziel; Hardwarezustand ist unklar.");
        }
        return ModeTransitionResult {.authoritative_mode = first, .changed = true};
    } catch (const EcWriteOutcomeError&) {
        throw;
    } catch (const std::exception& error) {
        throw EcWriteOutcomeError(
            "P-MODE wurde angefordert; autoritativer Readback fehlgeschlagen und der Hardwarezustand ist unklar: "
            + std::string(error.what()));
    }
}

EvoX2Probe::EvoX2Probe(IEcRegisterReader& reader)
    : reader_(reader)
{
}

Snapshot EvoX2Probe::read_snapshot()
{
    const std::uint8_t mode_before = reader_.read_byte(kPModeRegister);
    const std::uint8_t firmware_major = reader_.read_byte(kFirmwareMajorRegister);
    const std::uint8_t firmware_minor = reader_.read_byte(kFirmwareMinorRegister);
    const std::uint8_t temperature = reader_.read_byte(kTemperatureRegister);
    const std::uint8_t mode_after = reader_.read_byte(kPModeRegister);

    if (mode_before != mode_after) {
        throw EcProtocolError("P-MODE hat sich waehrend der Abfrage geaendert; bitte erneut ausfuehren.");
    }
    if ((firmware_major == 0 && firmware_minor == 0) || firmware_major == 0xFF || firmware_minor == 0xFF) {
        throw UnsupportedHardwareError("EC-Firmwarekennung ist unplausibel; kein AXB35-Zustand bestaetigt.");
    }
    if (temperature < 5 || temperature > 115) {
        throw UnsupportedHardwareError("EC-Temperatur ist unplausibel; kein AXB35-Zustand bestaetigt.");
    }

    return Snapshot {
        .mode = decode_mode(mode_before),
        .raw_mode = mode_before,
        .firmware_major = firmware_major,
        .firmware_minor = firmware_minor,
        .ec_temperature_celsius = temperature,
    };
}

std::string format_text(const Snapshot& snapshot)
{
    std::ostringstream output;
    output << "P-MODE: " << mode_name(snapshot.mode)
           << "\nRaw: " << hexadecimal_byte(snapshot.raw_mode)
           << "\nEC firmware: " << snapshot.firmware_version()
           << "\nEC temperature: " << static_cast<unsigned int>(snapshot.ec_temperature_celsius) << " C";
    return output.str();
}

std::string format_json(const Snapshot& snapshot)
{
    std::ostringstream output;
    output << "{\"mode\":\"" << mode_name_lower(snapshot.mode)
           << "\",\"raw\":" << static_cast<unsigned int>(snapshot.raw_mode)
           << ",\"ec_firmware\":\"" << snapshot.firmware_version()
           << "\",\"ec_temperature_c\":" << static_cast<unsigned int>(snapshot.ec_temperature_celsius) << '}';
    return output.str();
}

} // namespace evox2
