#include "coopstory/bridge/VersionGate.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace coopstory::bridge {
namespace {

struct FileCloser final {
    void operator()(void* value) const noexcept {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            ::CloseHandle(static_cast<HANDLE>(value));
        }
    }
};

using UniqueFile = std::unique_ptr<void, FileCloser>;

[[nodiscard]] std::string WindowsError(
    const std::string_view operation,
    const DWORD error = ::GetLastError()) {
    return std::string(operation) + " failed with Win32 error " +
           std::to_string(error);
}

[[nodiscard]] std::string NtError(
    const std::string_view operation,
    const NTSTATUS status) {
    std::ostringstream text;
    text << operation << " failed with NTSTATUS 0x" << std::hex
         << static_cast<unsigned long>(status);
    return text.str();
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const auto required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const auto written = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return written == required ? result : std::string{};
}

[[nodiscard]] bool CurrentModulePath(
    std::wstring& path,
    std::string& error) {
    std::vector<wchar_t> buffer(32'768U);
    const auto length = ::GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        error = WindowsError("GetModuleFileNameW");
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

[[nodiscard]] bool ReadFileVersion(
    const std::wstring& path,
    std::string& version,
    std::string& error) {
    DWORD ignored{};
    const auto size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0U) {
        error = WindowsError("GetFileVersionInfoSizeW");
        return false;
    }
    std::vector<std::uint8_t> bytes(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0U, size, bytes.data())) {
        error = WindowsError("GetFileVersionInfoW");
        return false;
    }

    VS_FIXEDFILEINFO* info{};
    UINT infoSize{};
    if (!::VerQueryValueW(
            bytes.data(),
            L"\\",
            reinterpret_cast<void**>(&info),
            &infoSize) ||
        info == nullptr || infoSize < sizeof(VS_FIXEDFILEINFO) ||
        info->dwSignature != 0xFEEF04BDU) {
        error = "RDR2.exe has no valid fixed file-version record";
        return false;
    }

    version =
        std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionLS));
    return true;
}

[[nodiscard]] bool Sha256File(
    const std::wstring& path,
    std::string& digest,
    std::string& error) {
    // Hash the actual executable in chunks so VersionGate can compare a stable identity before version-sensitive native bindings are used.
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    auto status = ::BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        error = NtError("BCryptOpenAlgorithmProvider", status);
        return false;
    }

    DWORD objectSize{};
    DWORD copied{};
    status = ::BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize),
        sizeof(objectSize),
        &copied,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        error = NtError("BCryptGetProperty(BCRYPT_OBJECT_LENGTH)", status);
        ::BCryptCloseAlgorithmProvider(algorithm, 0U);
        return false;
    }

    std::vector<std::uint8_t> hashObject(objectSize);
    status = ::BCryptCreateHash(
        algorithm,
        &hash,
        hashObject.data(),
        static_cast<ULONG>(hashObject.size()),
        nullptr,
        0U,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        error = NtError("BCryptCreateHash", status);
        ::BCryptCloseAlgorithmProvider(algorithm, 0U);
        return false;
    }

    UniqueFile file{
        ::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr)};
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = WindowsError("CreateFileW(RDR2.exe)");
        ::BCryptDestroyHash(hash);
        ::BCryptCloseAlgorithmProvider(algorithm, 0U);
        return false;
    }

    std::array<std::uint8_t, 64U * 1024U> buffer{};
    for (;;) {
        DWORD bytesRead{};
        if (!::ReadFile(
                static_cast<HANDLE>(file.get()),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead,
                nullptr)) {
            error = WindowsError("ReadFile(RDR2.exe)");
            ::BCryptDestroyHash(hash);
            ::BCryptCloseAlgorithmProvider(algorithm, 0U);
            return false;
        }
        if (bytesRead == 0U) {
            break;
        }
        status = ::BCryptHashData(hash, buffer.data(), bytesRead, 0U);
        if (!BCRYPT_SUCCESS(status)) {
            error = NtError("BCryptHashData", status);
            ::BCryptDestroyHash(hash);
            ::BCryptCloseAlgorithmProvider(algorithm, 0U);
            return false;
        }
    }

    std::array<std::uint8_t, 32> bytes{};
    status = ::BCryptFinishHash(
        hash,
        bytes.data(),
        static_cast<ULONG>(bytes.size()),
        0U);
    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0U);
    if (!BCRYPT_SUCCESS(status)) {
        error = NtError("BCryptFinishHash", status);
        return false;
    }

    std::ostringstream text;
    text << std::hex << std::uppercase << std::setfill('0');
    for (const auto value : bytes) {
        text << std::setw(2) << static_cast<unsigned int>(value);
    }
    digest = text.str();
    return true;
}

}  // namespace

GameIdentityProbeResult ProbeCurrentGameIdentity() {
    // Gather filename, version and content hash separately; an incomplete probe is returned to VersionGate as a safe denial instead of throwing into RDR2.
    GameIdentityProbeResult result;
    std::wstring path;
    if (!CurrentModulePath(path, result.error)) {
        return result;
    }

    result.identity.executableName =
        WideToUtf8(std::filesystem::path(path).filename().wstring());
    if (result.identity.executableName.empty()) {
        result.error = "failed to convert executable name to UTF-8";
        return result;
    }
    if (!ReadFileVersion(path, result.identity.fileVersion, result.error)) {
        return result;
    }
    if (!Sha256File(path, result.identity.sha256, result.error)) {
        return result;
    }

    result.succeeded = true;
    return result;
}

}  // namespace coopstory::bridge
