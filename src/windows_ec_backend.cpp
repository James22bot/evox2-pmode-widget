#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include "evox2/windows_ec_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace evox2::windows {
namespace {

constexpr wchar_t kPawnIoDevice[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr wchar_t kModuleFileName[] = L"LpcACPIEC.bin";
constexpr wchar_t kEcMutexName[] = L"Global\\Access_EC";
constexpr std::size_t kExpectedModuleSize = 2612;
constexpr std::uint8_t kPModeRegister = 0x31;
constexpr DWORD kPModeSettleMilliseconds = 200;
constexpr std::array<std::uint8_t, 32> kExpectedModuleSha256 {
    0xc3, 0x8f, 0xd1, 0x16, 0xe7, 0xaf, 0xf4, 0xd1,
    0xfd, 0xb0, 0xa4, 0x94, 0xe2, 0x96, 0xbe, 0x0a,
    0x67, 0x08, 0xe5, 0xa2, 0x2f, 0xc7, 0x2f, 0x14,
    0x58, 0x74, 0x42, 0xfb, 0x7f, 0x8f, 0x79, 0x06,
};

constexpr DWORD kPawnIoDeviceType = 41394u << 16;
constexpr DWORD kPawnIoLoadBinary = kPawnIoDeviceType | (0x821u << 2);
constexpr DWORD kPawnIoExecute = kPawnIoDeviceType | (0x841u << 2);
constexpr std::size_t kFunctionNameBytes = 32;

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle)
        : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            if (valid()) {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::string utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0) {
        return "<unreadable>";
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr)
        <= 0) {
        return "<unreadable>";
    }
    return result;
}

std::string windows_error_message(DWORD code)
{
    wchar_t buffer[512] = {};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        buffer,
        static_cast<DWORD>(std::size(buffer)),
        nullptr);
    std::wstring message = length == 0 ? L"Windows error" : std::wstring(buffer, length);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    std::ostringstream result;
    result << utf8(message) << " (" << code << ')';
    return result.str();
}

std::wstring executable_directory()
{
    std::vector<wchar_t> path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw DependencyError("Pfad der Overlay-EXE konnte nicht bestimmt werden.");
    }
    std::wstring full(path.data(), length);
    const auto separator = full.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        throw DependencyError("Verzeichnis der Overlay-EXE konnte nicht bestimmt werden.");
    }
    return full.substr(0, separator);
}

std::vector<std::uint8_t> read_module_file(const std::wstring& path)
{
    UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.valid()) {
        throw DependencyError("LpcACPIEC.bin fehlt neben der Overlay-EXE oder ist nicht lesbar: "
                              + windows_error_message(GetLastError()));
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.get(), &size)) {
        throw DependencyError("Groesse von LpcACPIEC.bin konnte nicht gelesen werden: "
                              + windows_error_message(GetLastError()));
    }
    if (size.QuadPart != static_cast<LONGLONG>(kExpectedModuleSize)) {
        throw DependencyError("LpcACPIEC.bin hat nicht die erwartete Groesse; Laden verweigert.");
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.QuadPart));
    DWORD bytes_read = 0;
    if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &bytes_read, nullptr)) {
        throw DependencyError("LpcACPIEC.bin konnte nicht gelesen werden: "
                              + windows_error_message(GetLastError()));
    }
    if (bytes_read != data.size()) {
        throw DependencyError("LpcACPIEC.bin wurde nicht vollstaendig gelesen.");
    }
    return data;
}

std::array<std::uint8_t, 32> sha256(const std::vector<std::uint8_t>& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD result_size = 0;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    std::vector<std::uint8_t> hash_object;

    auto ensure_success = [](NTSTATUS status, std::string_view operation) {
        if (status < 0) {
            std::ostringstream message;
            message << operation << " fehlgeschlagen: NTSTATUS 0x" << std::hex << static_cast<unsigned long>(status);
            throw DependencyError(message.str());
        }
    };

    try {
        ensure_success(
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider");
        ensure_success(
            BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &result_size,
                0),
            "BCryptGetProperty(OBJECT_LENGTH)");
        ensure_success(
            BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_size),
                sizeof(hash_size),
                &result_size,
                0),
            "BCryptGetProperty(HASH_LENGTH)");
        if (hash_size != 32) {
            throw DependencyError("Windows meldete eine unerwartete SHA-256-Laenge.");
        }

        hash_object.resize(object_size);
        ensure_success(
            BCryptCreateHash(
                algorithm,
                &hash,
                hash_object.data(),
                static_cast<ULONG>(hash_object.size()),
                nullptr,
                0,
                0),
            "BCryptCreateHash");
        ensure_success(
            BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0),
            "BCryptHashData");

        std::array<std::uint8_t, 32> digest = {};
        ensure_success(
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0),
            "BCryptFinishHash");
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return digest;
    } catch (...) {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        throw;
    }
}

HANDLE create_ec_mutex()
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;0x00100001;;;WD)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        throw EcProtocolError(
            "EC-Mutex-Sicherheitsbeschreibung konnte nicht erstellt werden: "
            + windows_error_message(GetLastError()));
    }

    SECURITY_ATTRIBUTES attributes {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor,
        .bInheritHandle = FALSE,
    };
    HANDLE handle = CreateMutexExW(
        &attributes,
        kEcMutexName,
        0,
        SYNCHRONIZE | MUTEX_MODIFY_STATE);
    const DWORD create_error = handle == nullptr ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (handle == nullptr) {
        throw EcProtocolError(
            "Globaler EC-Zugriffsschutz konnte nicht geoeffnet werden: "
            + windows_error_message(create_error));
    }
    return handle;
}

class EcMutexGuard final {
public:
    EcMutexGuard()
        : handle_(create_ec_mutex())
    {
        if (!handle_.valid()) {
            throw EcProtocolError(
                "Globaler EC-Zugriffsschutz konnte nicht geoeffnet werden: " + windows_error_message(GetLastError()));
        }
        const DWORD result = WaitForSingleObject(handle_.get(), 250);
        if (result == WAIT_ABANDONED) {
            ReleaseMutex(handle_.get());
            throw EcProtocolError("Vorheriger EC-Zugriff wurde abgebrochen; Aktualisierung verworfen.");
        }
        if (result != WAIT_OBJECT_0) {
            if (result == WAIT_TIMEOUT) {
                throw EcProtocolError("Ein anderes Hardwarewerkzeug belegt den EC.");
            }
            throw EcProtocolError(
                "EC-Zugriffsschutz konnte nicht erworben werden: " + windows_error_message(GetLastError()));
        }
        acquired_ = true;
    }

    ~EcMutexGuard()
    {
        if (acquired_) {
            ReleaseMutex(handle_.get());
        }
    }

    EcMutexGuard(const EcMutexGuard&) = delete;
    EcMutexGuard& operator=(const EcMutexGuard&) = delete;

private:
    UniqueHandle handle_;
    bool acquired_ = false;
};

class PawnIoPortIo final : public IPortIo {
public:
    explicit PawnIoPortIo(const std::vector<std::uint8_t>& module)
        : handle_(CreateFileW(
              kPawnIoDevice,
              GENERIC_READ | GENERIC_WRITE,
              FILE_SHARE_READ | FILE_SHARE_WRITE,
              nullptr,
              OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL,
              nullptr))
    {
        if (!handle_.valid()) {
            const DWORD code = GetLastError();
            if (code == ERROR_ACCESS_DENIED) {
                throw DependencyError("PawnIO verweigert Zugriff. Overlay einmal als Administrator starten oder PawnIO-Zugriffsrechte pruefen.");
            }
            throw DependencyError(
                "Offizielles PawnIO 2.2.0 ist nicht erreichbar: " + windows_error_message(code));
        }

        DWORD returned = 0;
        if (!DeviceIoControl(
                handle_.get(),
                kPawnIoLoadBinary,
                const_cast<std::uint8_t*>(module.data()),
                static_cast<DWORD>(module.size()),
                nullptr,
                0,
                &returned,
                nullptr)) {
            throw DependencyError(
                "Signiertes LpcACPIEC-PawnIO-Modul wurde abgelehnt: " + windows_error_message(GetLastError()));
        }
    }

    std::uint8_t read_port(std::uint8_t port) override
    {
        const auto output = execute("ioctl_pio_read", {port}, 1);
        return static_cast<std::uint8_t>(output[0] & 0xFFu);
    }

    void write_port(std::uint8_t port, std::uint8_t value) override
    {
        (void)execute("ioctl_pio_write", {port, value}, 0);
    }

private:
    std::vector<std::uint64_t> execute(
        std::string_view function,
        const std::vector<std::uint64_t>& arguments,
        std::size_t output_count)
    {
        if (function.size() >= kFunctionNameBytes) {
            throw EcProtocolError("PawnIO-Funktionsname ist zu lang.");
        }
        if (arguments.size() > (std::numeric_limits<DWORD>::max() - kFunctionNameBytes) / sizeof(std::uint64_t)
            || output_count > std::numeric_limits<DWORD>::max() / sizeof(std::uint64_t)) {
            throw EcProtocolError("PawnIO-Puffer ist zu gross.");
        }

        std::vector<std::uint8_t> input(kFunctionNameBytes + arguments.size() * sizeof(std::uint64_t), 0);
        std::memcpy(input.data(), function.data(), function.size());
        if (!arguments.empty()) {
            std::memcpy(
                input.data() + kFunctionNameBytes,
                arguments.data(),
                arguments.size() * sizeof(std::uint64_t));
        }

        std::vector<std::uint64_t> output(output_count, 0);
        DWORD bytes_returned = 0;
        if (!DeviceIoControl(
                handle_.get(),
                kPawnIoExecute,
                input.data(),
                static_cast<DWORD>(input.size()),
                output.empty() ? nullptr : output.data(),
                static_cast<DWORD>(output.size() * sizeof(std::uint64_t)),
                &bytes_returned,
                nullptr)) {
            throw EcProtocolError("PawnIO-Portoperation fehlgeschlagen: " + windows_error_message(GetLastError()));
        }
        if (bytes_returned != output.size() * sizeof(std::uint64_t)) {
            throw EcProtocolError("PawnIO lieferte eine unerwartete Antwortlaenge.");
        }
        return output;
    }

    UniqueHandle handle_;
};

class EcModeTransitionIo final : public IPModeTransitionIo {
public:
    explicit EcModeTransitionIo(IEcIo& io)
        : io_(io)
    {
    }

    Snapshot read_snapshot() override
    {
        AcpiEcRegisterReader registers(io_);
        return EvoX2Probe(registers).read_snapshot();
    }

    void write_mode(PMode mode) override
    {
        AcpiEcPModeWriter(io_).write_mode(mode);
    }

    void wait_for_mode_settle() override
    {
        Sleep(kPModeSettleMilliseconds);
    }

    PMode read_mode_once() override
    {
        AcpiEcRegisterReader registers(io_);
        return decode_mode(registers.read_byte(kPModeRegister));
    }

private:
    IEcIo& io_;
};

std::string registry_string(const wchar_t* value_name)
{
    wchar_t buffer[512] = {};
    DWORD bytes = sizeof(buffer);
    const LSTATUS result = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS",
        value_name,
        RRF_RT_REG_SZ,
        nullptr,
        buffer,
        &bytes);
    if (result != ERROR_SUCCESS) {
        return {};
    }
    const std::size_t characters = std::min<std::size_t>(bytes / sizeof(wchar_t), std::size(buffer));
    std::wstring value(buffer, characters);
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return utf8(value);
}

} // namespace

std::string BoardIdentity::describe() const
{
    std::ostringstream output;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << fields[index].first << '=' << (fields[index].second.empty() ? "<leer>" : fields[index].second);
    }
    return output.str();
}

BoardIdentity read_board_identity()
{
    BoardIdentity identity {
        .fields = {
            {"SystemManufacturer", registry_string(L"SystemManufacturer")},
            {"SystemProductName", registry_string(L"SystemProductName")},
            {"BaseBoardManufacturer", registry_string(L"BaseBoardManufacturer")},
            {"BaseBoardProduct", registry_string(L"BaseBoardProduct")},
        },
        .supported = false,
    };
    std::vector<std::string> values;
    values.reserve(identity.fields.size());
    for (const auto& [name, value] : identity.fields) {
        (void)name;
        values.push_back(value);
    }
    identity.supported = looks_like_supported_board(values);
    return identity;
}

class EcBackend::Impl final {
public:
    explicit Impl(const std::vector<std::uint8_t>& module)
        : ports(module), io(ports)
    {
    }

    PawnIoPortIo ports;
    AcpiEcPortIo io;
};

EcBackend::EcBackend()
{
    const BoardIdentity identity = read_board_identity();
    if (!identity.supported) {
        throw UnsupportedHardwareError("Board nicht eindeutig als EVO-X2/AXB35 erkannt: " + identity.describe());
    }

    std::wstring module_path = executable_directory();
    module_path += L'\\';
    module_path += kModuleFileName;
    const auto module = read_module_file(module_path);
    if (sha256(module) != kExpectedModuleSha256) {
        throw DependencyError("SHA-256 von LpcACPIEC.bin stimmt nicht; Laden verweigert.");
    }
    impl_ = std::make_unique<Impl>(module);
}

EcBackend::~EcBackend() = default;
EcBackend::EcBackend(EcBackend&&) noexcept = default;
EcBackend& EcBackend::operator=(EcBackend&&) noexcept = default;

Snapshot EcBackend::read_snapshot()
{
    EcMutexGuard guard;
    AcpiEcRegisterReader registers(impl_->io);
    return EvoX2Probe(registers).read_snapshot();
}

PMode EcBackend::read_mode()
{
    EcMutexGuard guard;
    AcpiEcRegisterReader registers(impl_->io);
    const std::uint8_t before = registers.read_byte(kPModeRegister);
    const std::uint8_t after = registers.read_byte(kPModeRegister);
    if (before != after) {
        throw EcProtocolError("P-MODE hat sich waehrend der Aktualisierung geaendert.");
    }
    return decode_mode(before);
}

ModeTransitionResult EcBackend::set_mode(PMode expected_current, PMode target)
{
    EcMutexGuard guard;
    EcModeTransitionIo transaction(impl_->io);
    return apply_mode_transition(transaction, expected_current, target);
}

} // namespace evox2::windows
