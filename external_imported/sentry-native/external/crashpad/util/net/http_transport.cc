// Copyright 2014 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/net/http_transport.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "util/net/http_body.h"

namespace crashpad {

HTTPTransport::HTTPTransport()
    : url_(),
      method_("POST"),
      headers_(),
      body_stream_(),
      timeout_(15.0) {
}

HTTPTransport::~HTTPTransport() {
}

// static
bool HTTPTransport::HandleHTTPStatus(unsigned long status_code) {
  if (status_code >= 200 && status_code <= 203) {
    return true;
  }

  switch (status_code) {
    case 413:
      LOG(ERROR) << "Crash report was discarded due to size limits "
                    "(HTTP 413 Content Too Large).";
      break;
    default:
      LOG(ERROR) << base::StringPrintf("HTTP status %lu", status_code);
      break;
  }
  return false;
}

void HTTPTransport::SetURL(const std::string& url) {
  url_ = url;
}

void HTTPTransport::SetHTTPProxy(const std::string& http_proxy) {
  http_proxy_ = http_proxy;
}

void HTTPTransport::SetMethod(const std::string& method) {
  method_ = method;
}

void HTTPTransport::SetHeader(const std::string& header,
                              const std::string& value) {
  headers_[header] = value;
}

void HTTPTransport::SetBodyStream(std::unique_ptr<HTTPBodyStream> stream) {
  body_stream_ = std::move(stream);
}

void HTTPTransport::SetTimeout(double timeout) {
  timeout_ = timeout;
}

void HTTPTransport::SetRootCACertificatePath(const base::FilePath& cert) {
  root_ca_certificate_path_ = cert;
}

}  // namespace crashpad
