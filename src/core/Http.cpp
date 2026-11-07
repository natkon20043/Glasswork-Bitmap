#include "Http.hpp"

#include <fstream>
#include <functional>

#ifdef _WIN32
#include "WindowsText.hpp"
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <mutex>
#endif

namespace gw {

namespace {

constexpr const char* kUserAgent = "GlassworkBitmap/1.0 (+https://open-vsx.org)";

#ifdef _WIN32

/// Downloads `url`, handing the body to `sink` piece by piece.
bool fetch(const std::string& url, const std::function<bool(const char*, size_t)>& sink, std::string* error) {
    std::wstring wideUrl = widenText(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        if (error) {
            *error = "not a web address: " + url;
        }
        return false;
    }
    std::wstring agent = widenText(kUserAgent);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session == nullptr) {
        if (error) {
            *error = windowsErrorMessage(GetLastError());
        }
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 180000);
    bool ok = false;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr) {
        std::wstring object = std::wstring(path) + extra;
        DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }
    if (request != nullptr && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status < 200 || status >= 300) {
            if (error) {
                *error = "HTTP " + std::to_string(status);
            }
        } else {
            ok = true;
            std::string chunk(64 * 1024, '\0');
            while (true) {
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                    ok = false;
                    if (error) {
                        *error = windowsErrorMessage(GetLastError());
                    }
                    break;
                }
                if (read == 0) {
                    break;
                }
                if (!sink(chunk.data(), read)) {
                    ok = false;
                    if (error) {
                        *error = "couldn't save the download";
                    }
                    break;
                }
            }
        }
    } else if (error) {
        *error = windowsErrorMessage(GetLastError());
    }
    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return ok;
}

#else

size_t deliver(char* data, size_t size, size_t count, void* target) {
    auto& sink = *static_cast<const std::function<bool(const char*, size_t)>*>(target);
    return sink(data, size * count) ? size * count : 0;
}

bool fetch(const std::string& url, const std::function<bool(const char*, size_t)>& sink, std::string* error) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error) {
            *error = "couldn't start a download";
        }
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, deliver);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, const_cast<std::function<bool(const char*, size_t)>*>(&sink));
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        if (error) {
            *error = curl_easy_strerror(result);
        }
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) {
            *error = "HTTP " + std::to_string(status);
        }
        return false;
    }
    return true;
}

#endif

}  // namespace

std::optional<std::string> httpGet(const std::string& url, std::string* error) {
    std::string body;
    bool ok = fetch(url, [&body](const char* data, size_t size) {
        body.append(data, size);
        return true;
    }, error);
    if (!ok) {
        return std::nullopt;
    }
    return body;
}

bool httpDownload(const std::string& url, const std::filesystem::path& file, std::string* error) {
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        if (error) {
            *error = "couldn't write " + file.filename().string();
        }
        return false;
    }
    bool ok = fetch(url, [&out](const char* data, size_t size) {
        out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    }, error);
    out.close();
    return ok && static_cast<bool>(out);
}

}  // namespace gw
