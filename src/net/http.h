// Thin libcurl wrapper: JSON/body requests, multipart upload, streaming and cancellation.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace http {

struct Part {
    std::string name, filename, content_type, data;
};

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;       // "Name: value"
    std::string body;
    std::vector<Part> multipart;            // if non-empty, sent as multipart/form-data
    long timeout_s = 60;
    long connect_timeout_s = 10;
    std::atomic<bool> *cancel = nullptr;    // set to true from another thread to abort
    // When set, body bytes are handed over as they arrive instead of being collected.
    std::function<void(const char *, size_t)> on_data;
};

struct Response {
    long status = 0;
    std::string body;
    std::string error;                       // transport-level failure, empty if none
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
    std::string describe() const;            // short human-readable failure reason
};

void global_init();
Response send(const Request &req);
std::string url_encode(const std::string &s);

}  // namespace http
