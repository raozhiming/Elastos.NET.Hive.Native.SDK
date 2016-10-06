/*
Copyright (c) 2016-2016, Vasil Dimov

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <nlohmann/json.hpp>
#include <ostream>
#include <string>

#include <ipfs/api.h>

namespace ipfs {

Ipfs::Ipfs(const std::string& host, long port, Protocol)
    : host_("http://" + host + ":" + std::to_string(port) + "/api/v0"),
      port_(port) {}

void Ipfs::Id(Json* response) {
  HttpResponseString http_response;

  const std::string url = host_ + "/id?stream-channels=true";

  http_.Fetch(url, &http_response);

  *response = Json::parse(http_response.body);
}

void Ipfs::Version(Json* response) {
  HttpResponseString http_response;

  const std::string url = host_ + "/version?stream-channels=true";

  http_.Fetch(url, &http_response);

  *response = Json::parse(http_response.body);
}

void Ipfs::Get(const std::string& hash, std::ostream* response) {
  HttpResponseStream http_response;
  http_response.body = response;

  const std::string url = host_ + "/cat?stream-channels=true&arg=" + hash;

  http_.Stream(url, &http_response);
}
}