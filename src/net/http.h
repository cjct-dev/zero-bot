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

    // For devices on the home network (smart-home drivers). A request that sets any of the TLS
    // options gets a connection of its own, never one shared with other requests.
    //
    // `local` marks a request to a device: it never goes through a proxy (even one set in the
    // environment), a redirect is returned as it is rather than followed (a device's key in a
    // header must not travel to another host), and only http and https are spoken. Setting any
    // of the device-only options below (insecure, pinned_pubkey, want_peer_cert, digest or a
    // client certificate) implies it.
    bool local = false;
    long timeout_ms = 0, connect_timeout_ms = 0;   // when set, used instead of the _s values
    bool insecure = false;                  // accept any certificate: a device's own self-signed one
    std::string pinned_pubkey;              // "sha256//<base64>": only a peer with this public key (checked even when insecure)
    bool want_peer_cert = false;            // fill Response::peer_cert_pem, for pinning on first contact
    bool no_redirect = false;               // a redirect is returned as it is, not followed
    std::string digest_user, digest_pass;   // HTTP digest authentication
    std::string client_cert_pem, client_key_pem;   // a client certificate and its key, as PEM text
};

struct Response {
    long status = 0;
    std::string body;
    std::string error;                       // transport-level failure, empty if none
    std::string peer_cert_pem;               // with want_peer_cert: the peer's own certificate
    std::string content_type;                // the answer's Content-Type header, as sent ("" if none)
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
    std::string describe() const;            // short human-readable failure reason
};

void global_init();
Response send(const Request &req);
std::string url_encode(const std::string &s);

}  // namespace http
