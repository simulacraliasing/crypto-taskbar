#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "core.hpp"
#include <chrono>
#include <memory>
#include <windows.h>
#include <winhttp.h>

namespace ct {
inline std::wstring Wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
inline std::string Utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}
struct InternetCloser {
    void operator()(void *p) const {
        if (p)
            WinHttpCloseHandle(p);
    }
};
using Internet = std::unique_ptr<void, InternetCloser>;
inline std::wstring ExpandHeader(const std::string &text) {
    const auto w = Wide(text);
    DWORD size = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (!size || size > 32768)
        throw std::runtime_error("Header environment expansion failed.");
    std::wstring out(size, 0);
    if (!ExpandEnvironmentStringsW(w.c_str(), out.data(), size))
        throw std::runtime_error("Header environment expansion failed.");
    out.resize(size - 1);
    if (out.find_first_of(L"\r\n") != std::wstring::npos)
        throw std::runtime_error("Invalid expanded header.");
    return out;
}
inline Json FetchJson(HINTERNET session, const std::string &url, const Source &src) {
    auto wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP))
        throw std::runtime_error("Invalid source URL.");
    if (parts.dwUserNameLength || parts.dwPasswordLength)
        throw std::runtime_error("Use headers for authentication, not URL credentials.");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path.empty())
        path = L"/";
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (!connection)
        throw std::runtime_error("Cannot connect to data source.");
    Internet request(
        WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                           parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request)
        throw std::runtime_error("Cannot create HTTP request.");
    WinHttpSetTimeouts(request.get(), 2000, 3000, 3000, 4000);
    // Never forward configured authentication headers to a redirect destination.
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    std::wstring headers = L"Accept: application/json\r\n";
    for (const auto &[name, value] : src.headers)
        headers += Wide(name) + L": " + ExpandHeader(value) + L"\r\n";
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        throw std::runtime_error("Network error " + std::to_string(GetLastError()) + ".");
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &length, nullptr))
        throw std::runtime_error("Missing HTTP status.");
    if (status != 200)
        throw std::runtime_error("HTTP " + std::to_string(status));
    std::string body;
    const auto start = std::chrono::steady_clock::now();
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &read))
            throw std::runtime_error("Failed to read response.");
        if (!read)
            break;
        body.append(buffer, read);
        if (body.size() > 2 * 1024 * 1024)
            throw std::runtime_error("Response exceeds 2 MiB.");
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(8))
            throw std::runtime_error("Response timed out.");
    }
    auto json = Json::parse(body, nullptr, false);
    if (json.is_discarded())
        throw std::runtime_error("Source returned invalid JSON.");
    return json;
}
inline Internet OpenSession() {
    return Internet(WinHttpOpen(L"CryptoTaskbar/1.0 "
                                L"(+https://github.com/simulacraliasing/crypto-taskbar)",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0));
}
} // namespace ct
