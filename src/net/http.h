// Thin libcurl wrapper: JSON/body requests, multipart upload, streaming and cancellation.
#pragma once
#include <atomic>
#include <functional>
#include <map>
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
    // A second flag that aborts as well: the caller's own (ESC), beside one the request's own code
    // sets (the radio's check ends a stream once enough has come, through `cancel`).
    std::atomic<bool> *cancel_too = nullptr;
    // When set, body bytes are handed over as they arrive instead of being collected.
    std::function<void(const char *, size_t)> on_data;
    // Reply headers to hand back in Response::headers, by name in any case (a search service's
    // allowance left, say). Only the last answer's: not a redirect's or a challenge's before it.
    std::vector<std::string> keep_headers;

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
    long status = 0;                         // the last answer's, kept when the transfer then failed (a body cut short)
    std::string body;
    std::string error;                       // transport-level failure, empty if none
    std::string peer_cert_pem;               // with want_peer_cert: the peer's own certificate
    std::string content_type;                // the answer's Content-Type header, as sent ("" if none)
    std::map<std::string, std::string> headers;   // those Request::keep_headers asked for, by lower-case name, trimmed
    bool sent = false;                       // a connection was made and the request went out on it
    // With digest_user: the last answer is to the request that carried the login, not the challenge
    // before it (libcurl sends the login only once challenged). A 401 then refused the login; a
    // challenge, even one whose body was cut short, refused nothing.
    bool digest_answered = false;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
    std::string describe() const;            // short human-readable failure reason
};

void global_init();
Response send(const Request &req);
std::string url_encode(const std::string &s);

// Whether a failed request may simply be sent again: the connection could not be made or broke,
// or the server said it is overloaded or failing for the moment (408, 429, 5xx but 501 and 505).
// Never a refusal or any other 4xx, a cancellation, or a timeout once the request had gone out
// (its time is up, and waiting again would double it).
bool transient(const Response &res);

// For requests that are safe to repeat and nothing else: reading, searching, asking the AI
// service. Never for one that changes something, logs in or pairs. Sends `req`, and again after a
// pause while the failure is transient (or `again` says so), at most `attempts` times, all inside
// `budget_ms` from the first send: each send's timeout is cut to what is left, and no new send
// starts with less than `min_ms` left. Cancelling during a pause ends it at once ("cancelled").
struct Retry {
    int attempts = 2;
    long budget_ms = 20000;
    long min_ms = 2000;
    int pause_ms = 500;                                  // before the first resend, doubled for each later one
    std::function<bool(const Response &)> again;         // default: !ok() && transient()
    std::function<void()> before_resend;                 // forget what on_data collected from the failed send
};
Response send_retrying(Request req, const Retry &retry);

}  // namespace http
