// Copyright 2006 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

#if defined(HAVE_ZLIB)
#include <zlib.h>
#endif

// Disable exception handler warnings.
#pragma warning(disable:4530)

#include "common/windows/string_utils-inl.h"

#include "common/windows/http_upload.h"

namespace {
  using std::string;
  using std::wstring;
  using std::map;
  using std::unique_ptr;

// Silence warning C4100, which may strike when building without zlib support.
#pragma warning(push)
#pragma warning(disable:4100)

  // Compresses the contents of `data` into `deflated` using the deflate
  // algorithm, if supported. Returns true on success, or false if not supported
  // or in case of any error. The contents of `deflated` are undefined in the
  // latter case.
  bool Deflate(const string& data, string& deflated) {
#if defined(HAVE_ZLIB)
    z_stream stream{};

    // Start with an output buffer sufficient for 75% compression to avoid
    // reallocations.
    deflated.resize(data.size() / 4);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = data.size();
    stream.next_out = reinterpret_cast<Bytef*>(&deflated[0]);
    stream.avail_out = deflated.size();
    stream.data_type = Z_TEXT;

    // Z_BEST_SPEED is chosen because, in practice, it offers excellent speed
    // with comparable compression for the symbol data typically being uploaded.
    // Z_BEST_COMPRESSION:    2151202094 bytes compressed 84.27% in 74.440s.
    // Z_DEFAULT_COMPRESSION: 2151202094 bytes compressed 84.08% in 36.016s.
    // Z_BEST_SPEED:          2151202094 bytes compressed 80.39% in 13.73s.
    int result = deflateInit(&stream, Z_BEST_SPEED);
    if (result != Z_OK) {
      return false;
    }

    while (true) {
      result = deflate(&stream, /*flush=*/Z_FINISH);
      if (result == Z_STREAM_END) {  // All data processed.
        deflated.resize(stream.total_out);
        break;
      }
      if (result != Z_OK && result != Z_BUF_ERROR) {
        fwprintf(stderr, L"Compression failed with zlib error %d\n", result);
        break;  // Error condition.
      }
      // Grow `deflated` by at least 1k to accept the rest of the data.
      deflated.resize(deflated.size() + std::max(stream.avail_in, 1024U));
      stream.next_out = reinterpret_cast<Bytef*>(&deflated[stream.total_out]);
      stream.avail_out = deflated.size() - stream.total_out;
    }
    deflateEnd(&stream);

    return result == Z_STREAM_END;
#else
    return false;
#endif  // defined(HAVE_ZLIB)
  }

// Restore C4100 to its previous state.
#pragma warning(pop)

  const wchar_t kUserAgent[] = L"Breakpad/1.0 (Windows)";

  // Helper class which closes an internet handle when it goes away
  class AutoInternetHandle {
  public:
    explicit AutoInternetHandle(HINTERNET handle) : handle_(handle) {}
    ~AutoInternetHandle() {
      if (handle_) {
        InternetCloseHandle(handle_);
      }
    }

    HINTERNET get() { return handle_; }

  private:
    HINTERNET handle_;
  };

  wstring UTF8ToWide(const string& utf8) {
    if (utf8.length() == 0) {
      return wstring();
    }

    // compute the length of the buffer we'll need
    int charcount = MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);

    if (charcount == 0) {
      return wstring();
    }

    // convert
    wchar_t* buf = new wchar_t[charcount];
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, charcount);
    wstring result(buf);
    delete[] buf;
    return result;
  }

  string WideToMBCP(const wstring& wide, unsigned int cp) {
    if (wide.length() == 0) {
      return string();
    }

    // compute the length of the buffer we'll need
    int charcount = WideCharToMultiByte(cp, 0, wide.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (charcount == 0) {
      return string();
    }

    // convert
    char *buf = new char[charcount];
    WideCharToMultiByte(cp, 0, wide.c_str(), -1, buf, charcount,
        nullptr, nullptr);

    string result(buf);
    delete[] buf;
    return result;
  }

  // Returns a string representation of a given Windows error code, or null
  // on failure.
  using ScopedLocalString = unique_ptr<wchar_t, decltype(&LocalFree)>;
  ScopedLocalString FormatError(DWORD error) {
    wchar_t* message_buffer = nullptr;
    DWORD message_length =
        ::FormatMessageW(
             FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
             FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
             /*lpSource=*/::GetModuleHandle(L"wininet.dll"), error,
             /*dwLanguageId=*/0, reinterpret_cast<wchar_t*>(&message_buffer),
             /*nSize=*/0, /*Arguments=*/nullptr);
    return ScopedLocalString(message_length ? message_buffer : nullptr,
                             &LocalFree);
  }

  // Emits a log message to stderr for the named operation and Windows error
  // code.
  void LogError(const char* operation, DWORD error) {
    ScopedLocalString message = FormatError(error);
    fwprintf(stderr, L"%S failed with error %u: %s\n", operation, error,
             message ? message.get() : L"");
  }

  // Invokes the Win32 CloseHandle function on `handle` if it is valid.
  // Intended for use as a deleter for a std::unique_ptr.
  void CloseWindowsHandle(void* handle) {
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
      ::CloseHandle(handle);
    }
  }

  // Appends the contents of the file at `filename` to `contents`.
  bool AppendFileContents(const wstring& filename, string* contents) {
    // Use Win32 APIs rather than the STL so that files larger than 2^31-1 bytes
    // can be read. This also allows for use of a hint to the cache manager that
    // the file will be read sequentially, which can improve performance.
    using ScopedWindowsHandle =
        unique_ptr<void, decltype(&CloseWindowsHandle)>;
    ScopedWindowsHandle file(
        ::CreateFileW(filename.c_str(), GENERIC_READ,
                      FILE_SHARE_DELETE | FILE_SHARE_READ,
                      /*lpSecurityAttributes=*/nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                      /*hTemplateFile=*/nullptr), &CloseWindowsHandle);
    BY_HANDLE_FILE_INFORMATION info = {};
    if (file.get() != nullptr && file.get() != INVALID_HANDLE_VALUE &&
        ::GetFileInformationByHandle(file.get(), &info)) {
      uint64_t file_size = info.nFileSizeHigh;
      file_size <<= 32;
      file_size |= info.nFileSizeLow;
      string::size_type position = contents->size();
      contents->resize(position + file_size);
      constexpr DWORD kChunkSize = 1024*1024;
      while (file_size) {
        const DWORD bytes_to_read =
            (file_size >= kChunkSize
                ? kChunkSize
                : static_cast<DWORD>(file_size));
        DWORD bytes_read = 0;
        if (!::ReadFile(file.get(), &((*contents)[position]), bytes_to_read,
                        &bytes_read, /*lpOverlapped=*/nullptr)) {
          return false;
        }
        position += bytes_read;
        file_size -= bytes_read;
      }
    }
    return true;
  }

  bool CheckParameters(const map<wstring, wstring>& parameters) {
    for (map<wstring, wstring>::const_iterator pos = parameters.begin();
          pos != parameters.end(); ++pos) {
      const wstring& str = pos->first;
      if (str.size() == 0) {
        return false;  // disallow empty parameter names
      }
      for (unsigned int i = 0; i < str.size(); ++i) {
        wchar_t c = str[i];
        if (c < 32 || c == '"' || c > 127) {
          return false;
        }
      }
    }
    return true;
  }

  // Converts a UTF16 string to UTF8.
  string WideToUTF8(const wstring& wide) {
    return WideToMBCP(wide, CP_UTF8);
  }

  bool ReadResponse(HINTERNET request, wstring *response) {
    bool has_content_length_header = false;
    wchar_t content_length[32];
    DWORD content_length_size = sizeof(content_length);
    DWORD claimed_size = 0;
    string response_body;

    if (HttpQueryInfo(request, HTTP_QUERY_CONTENT_LENGTH,
        static_cast<LPVOID>(&content_length),
        &content_length_size, 0)) {
      has_content_length_header = true;
      claimed_size = wcstol(content_length, nullptr, 10);
      response_body.reserve(claimed_size);
    } else {
      DWORD error = ::GetLastError();
      if (error != ERROR_HTTP_HEADER_NOT_FOUND) {
        LogError("HttpQueryInfo", error);
      }
    }

    DWORD total_read = 0;
    while (true) {
      DWORD bytes_available;
      if (!InternetQueryDataAvailable(request, &bytes_available, 0, 0)) {
        LogError("InternetQueryDataAvailable", ::GetLastError());
        return false;
      }
      if (bytes_available == 0) {
        break;
      }
      // Grow the output to hold the available bytes.
      response_body.resize(total_read + bytes_available);
      DWORD size_read;
      if (!InternetReadFile(request, &response_body[total_read],
                            bytes_available, &size_read)) {
        LogError("InternetReadFile", ::GetLastError());
        return false;
      }
      if (size_read == 0) {
        break;
      }
      total_read += size_read;
    }
    // The body may have been over-sized above; shrink to the actual bytes read.
    response_body.resize(total_read);

    if (has_content_length_header && (total_read != claimed_size)) {
      return false;  // The response doesn't match the Content-Length header.
    }
    if (response) {
      *response = UTF8ToWide(response_body);
    }
    return true;
  }

  bool SendRequestInner(
      const wstring& url,
      const wstring& http_method,
      const wstring& content_type_header,
      const string& request_body,
      int* timeout_ms,
      wstring* response_body,
      int* response_code) {
    if (response_code) {
      *response_code = 0;
    }

    // Break up the URL and make sure we can handle it
    wchar_t scheme[16], host[256], path[1024];
    URL_COMPONENTS components;
    memset(&components, 0, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.lpszScheme = scheme;
    components.dwSchemeLength = sizeof(scheme) / sizeof(scheme[0]);
    components.lpszHostName = host;
    components.dwHostNameLength = sizeof(host) / sizeof(host[0]);
    components.lpszUrlPath = path;
    components.dwUrlPathLength = sizeof(path) / sizeof(path[0]);
    if (!InternetCrackUrl(url.c_str(), static_cast<DWORD>(url.size()),
        0, &components)) {
      LogError("InternetCrackUrl", ::GetLastError());
      return false;
    }
    bool secure = false;
    if (wcscmp(scheme, L"https") == 0) {
      secure = true;
    }
    else if (wcscmp(scheme, L"http") != 0) {
      return false;
    }

    AutoInternetHandle internet(InternetOpen(kUserAgent,
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,  // proxy name
        nullptr,  // proxy bypass
        0));   // flags
    if (!internet.get()) {
      LogError("InternetOpen", ::GetLastError());
      return false;
    }

    AutoInternetHandle connection(InternetConnect(internet.get(),
        host,
        components.nPort,
        nullptr,    // user name
        nullptr,    // password
        INTERNET_SERVICE_HTTP,
        0,       // flags
        0));  // context
    if (!connection.get()) {
      LogError("InternetConnect", ::GetLastError());
      return false;
    }

    DWORD http_open_flags = secure ? INTERNET_FLAG_SECURE : 0;
    http_open_flags |= INTERNET_FLAG_NO_COOKIES;
    AutoInternetHandle request(HttpOpenRequest(connection.get(),
        http_method.c_str(),
        path,
        nullptr,    // version
        nullptr,    // referer
        nullptr,    // agent type
        http_open_flags,
        0));  // context
    if (!request.get()) {
      LogError("HttpOpenRequest", ::GetLastError());
      return false;
    }

    if (!content_type_header.empty()) {
      if (!HttpAddRequestHeaders(request.get(),
                                 content_type_header.c_str(),
                                 static_cast<DWORD>(-1),
                                 HTTP_ADDREQ_FLAG_ADD)) {
        LogError("HttpAddRequestHeaders", ::GetLastError());
      }
    }

    if (timeout_ms) {
      if (!InternetSetOption(request.get(),
          INTERNET_OPTION_SEND_TIMEOUT,
          timeout_ms,
          sizeof(*timeout_ms))) {
        LogError("InternetSetOption-send timeout", ::GetLastError());
      }

      if (!InternetSetOption(request.get(),
          INTERNET_OPTION_RECEIVE_TIMEOUT,
          timeout_ms,
          sizeof(*timeout_ms))) {
        LogError("InternetSetOption-receive timeout", ::GetLastError());
      }
    }

    if (!HttpSendRequest(request.get(), nullptr, 0,
        const_cast<char*>(request_body.data()),
        static_cast<DWORD>(request_body.size()))) {
      LogError("HttpSendRequest", ::GetLastError());
      return false;
    }

    // The server indicates a successful upload with HTTP status 200.
    wchar_t http_status[4];
    DWORD http_status_size = sizeof(http_status);
    if (!HttpQueryInfo(request.get(), HTTP_QUERY_STATUS_CODE,
        static_cast<LPVOID>(&http_status), &http_status_size,
        0)) {
      LogError("HttpQueryInfo", ::GetLastError());
      return false;
    }

    int http_response = wcstol(http_status, nullptr, 10);
    if (response_code) {
      *response_code = http_response;
    }

    bool result = (http_response == 200);

    if (result) {
      result = ReadResponse(request.get(), response_body);
    }

    return result;
  }

  wstring GenerateMultipartBoundary() {
    // The boundary has 27 '-' characters followed by 16 hex digits
    static const wchar_t kBoundaryPrefix[] = L"---------------------------";
    static const int kBoundaryLength = 27 + 16 + 1;

    // Generate some random numbers to fill out the boundary
    int r0 = rand();
    int r1 = rand();

    wchar_t temp[kBoundaryLength];
    swprintf(temp, kBoundaryLength, L"%s%08X%08X", kBoundaryPrefix, r0, r1);

    // remove when VC++7.1 is no longer supported
    temp[kBoundaryLength - 1] = L'\0';

    return wstring(temp);
  }

  wstring GenerateMultipartPostRequestHeader(const wstring& boundary) {
    wstring header = L"Content-Type: multipart/form-data; boundary=";
    header += boundary;
    return header;
  }

  bool AppendFileToRequestBody(const wstring& file_part_name,
                               const wstring& filename,
                               string* request_body,
                               bool set_content_type = true) {
    string file_part_name_utf8 = WideToUTF8(file_part_name);
    if (file_part_name_utf8.empty()) {
      return false;
    }

    string filename_utf8 = WideToUTF8(filename);
    if (filename_utf8.empty()) {
      return false;
    }

    if (set_content_type) {
      request_body->append(
          "Content-Disposition: form-data; "
          "name=\"" +
          file_part_name_utf8 +
          "\"; "
          "filename=\"" +
          filename_utf8 + "\"\r\n");
      request_body->append("Content-Type: application/octet-stream\r\n");
      request_body->append("\r\n");
    }

    return AppendFileContents(filename, request_body);
  }

  bool GenerateRequestBody(const map<wstring, wstring>& parameters,
      const map<wstring, wstring>& files,
      const wstring& boundary,
      string *request_body) {
    string boundary_str = WideToUTF8(boundary);
    if (boundary_str.empty()) {
      return false;
    }

    request_body->clear();

    // Append each of the parameter pairs as a form-data part
    for (map<wstring, wstring>::const_iterator pos = parameters.begin();
        pos != parameters.end(); ++pos) {
      request_body->append("--" + boundary_str + "\r\n");
      request_body->append("Content-Disposition: form-data; name=\"" +
          WideToUTF8(pos->first) + "\"\r\n\r\n" +
          WideToUTF8(pos->second) + "\r\n");
    }

    // Now append each upload file as a binary (octet-stream) part
    for (map<wstring, wstring>::const_iterator pos = files.begin();
        pos != files.end(); ++pos) {
      request_body->append("--" + boundary_str + "\r\n");

      if (!AppendFileToRequestBody(pos->first, pos->second, request_body)) {
        return false;
      }
    }
    request_body->append("--" + boundary_str + "--\r\n");
    return true;
  }
}

namespace google_breakpad {
  bool HTTPUpload::SendPutRequest(
      const wstring& url,
      const wstring& path,
      int* timeout_ms,
      wstring* response_body,
      int* response_code) {
    string request_body;
    // Turn off content-type in the body. If content-type is set then binary
    // files uploaded to GCS end up with the it prepended to the file
    // contents.
    if (!AppendFileToRequestBody(L"symbol_file", path, &request_body,
                                 /*set_content_type=*/false)) {
      return false;
    }

    static const wchar_t kNoEncoding[] = L"";
    static const wchar_t kDeflateEncoding[] = L"Content-Encoding: deflate\r\n";
    const wchar_t* encoding_header = &kNoEncoding[0];
    string compressed_body;
    if (Deflate(request_body, compressed_body)) {
      request_body.swap(compressed_body);
      encoding_header = &kDeflateEncoding[0];
    }  // else deflate unsupported or failed; send the raw data.
    string().swap(compressed_body);  // Free memory.

    return SendRequestInner(
        url,
        L"PUT",
        encoding_header,
        request_body,
        timeout_ms,
        response_body,
        response_code);
  }

  bool HTTPUpload::SendGetRequest(
      const wstring& url,
      int* timeout_ms,
      wstring* response_body,
      int* response_code) {
    return SendRequestInner(
        url,
        L"GET",
        L"",
        "",
        timeout_ms,
        response_body,
        response_code);
  }

  bool HTTPUpload::SendMultipartPostRequest(
      const wstring& url,
      const map<wstring, wstring>& parameters,
      const map<wstring, wstring>& files,
      int* timeout_ms,
      wstring* response_body,
      int* response_code) {
    // TODO(bryner): support non-ASCII parameter names
    if (!CheckParameters(parameters)) {
      return false;
    }

    wstring boundary = GenerateMultipartBoundary();
    wstring content_type_header = GenerateMultipartPostRequestHeader(boundary);

    string request_body;
    if (!GenerateRequestBody(parameters, files, boundary, &request_body)) {
      return false;
    }

    return SendRequestInner(
        url,
        L"POST",
        content_type_header,
        request_body,
        timeout_ms,
        response_body,
        response_code);
  }

  bool HTTPUpload::SendSimplePostRequest(
      const wstring& url,
      const wstring& body,
      const wstring& content_type,
      int *timeout_ms,
      wstring *response_body,
      int *response_code) {
    return SendRequestInner(
        url,
        L"POST",
        content_type,
        WideToUTF8(body),
        timeout_ms,
        response_body,
        response_code);
  }
}  // namespace google_breakpad
