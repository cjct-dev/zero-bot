#include "net/http.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <curl/curl.h>

namespace http {

namespace {
struct Ctx {
    const Request *req;
    Response *res;
};

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *ctx = static_cast<Ctx *>(userdata);
    const size_t n = size * nmemb;
    // header_cb marks the response: -1 = success (stream it), -2 = error (collect the body).
    if (ctx->req->on_data && ctx->res->status != -2) {
        ctx->req->on_data(ptr, n);
    } else if (ctx->res->body.size() < 8 * 1024 * 1024) {
        ctx->res->body.append(ptr, n);
    }
    return n;
}

size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    // Switch to "collect" mode for non-2xx responses to streaming requests.
    auto *ctx = static_cast<Ctx *>(userdata);
    const size_t n = size * nmemb;
    if (n > 12 && !strncmp(ptr, "HTTP/", 5)) {
        const char *sp = static_cast<const char *>(memchr(ptr, ' ', n));
        const int code = sp ? atoi(sp + 1) : 0;
        ctx->res->status = (code >= 200 && code < 300) ? -1 : -2;
    }
    return n;
}

int progress_cb(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *ctx = static_cast<Ctx *>(userdata);
    const http::Request &req = *ctx->req;
    return (req.cancel && req.cancel->load()) || (req.cancel_too && req.cancel_too->load()) ? 1 : 0;
}
}  // namespace

// Every request used to open a fresh connection, paying for DNS, TCP and TLS set-up each time.
// That costs a few hundred milliseconds on Wi-Fi and seconds on a Bluetooth-tethered phone, so
// connections, DNS answers and TLS sessions are shared between all requests and threads.
CURLSH *g_share = nullptr;
std::mutex g_share_locks[CURL_LOCK_DATA_LAST];

void share_lock(CURL *, curl_lock_data data, curl_lock_access, void *) { g_share_locks[data].lock(); }
void share_unlock(CURL *, curl_lock_data data, void *) { g_share_locks[data].unlock(); }

void global_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_share = curl_share_init();
    if (!g_share) return;
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

std::string Response::describe() const {
    if (!error.empty()) return error;
    std::string msg = "HTTP " + std::to_string(status);
    if (!body.empty()) {
        std::string snippet = body.substr(0, 160);
        for (char &c : snippet)
            if (c == '\n' || c == '\r') c = ' ';
        msg += ": " + snippet;
    }
    return msg;
}

Response send(const Request &req) {
    Response res;
    CURL *curl = curl_easy_init();
    if (!curl) { res.error = "could not start HTTP client"; return res; }
    Ctx ctx{&req, &res};

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    // A device's own certificate settings are kept apart from the shared connections and TLS
    // sessions used for the internet.
    const bool own_tls = req.insecure || !req.pinned_pubkey.empty() || !req.client_cert_pem.empty() || req.want_peer_cert;
    const bool local = req.local || own_tls || !req.digest_user.empty();
    if (g_share && !own_tls) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 600L);   // addresses are remembered for ten minutes, not one
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.no_redirect || local ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    if (local) {
        // A device on the home network: straight to it, whatever proxy the environment names,
        // and in plain or TLS HTTP only.
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (req.timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    else curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_s);
    if (req.connect_timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, req.connect_timeout_ms);
    else curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req.connect_timeout_s);
    if (req.insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!req.pinned_pubkey.empty()) curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, req.pinned_pubkey.c_str());
    if (req.want_peer_cert) curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);
    if (!req.digest_user.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_DIGEST));
        curl_easy_setopt(curl, CURLOPT_USERNAME, req.digest_user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, req.digest_pass.c_str());
    }
    if (!req.client_cert_pem.empty()) {
        curl_blob cert{const_cast<char *>(req.client_cert_pem.data()), req.client_cert_pem.size(), CURL_BLOB_COPY};
        curl_easy_setopt(curl, CURLOPT_SSLCERT_BLOB, &cert);
        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
        if (!req.client_key_pem.empty()) {
            curl_blob key{const_cast<char *>(req.client_key_pem.data()), req.client_key_pem.size(), CURL_BLOB_COPY};
            curl_easy_setopt(curl, CURLOPT_SSLKEY_BLOB, &key);
            curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
        }
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "zero-bot/" ZB_VERSION);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    curl_slist *headers = nullptr;
    for (auto &h : req.headers) headers = curl_slist_append(headers, h.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_mime *mime = nullptr;
    if (!req.multipart.empty()) {
        mime = curl_mime_init(curl);
        for (auto &p : req.multipart) {
            curl_mimepart *part = curl_mime_addpart(mime);
            curl_mime_name(part, p.name.c_str());
            curl_mime_data(part, p.data.data(), p.data.size());
            if (!p.filename.empty()) curl_mime_filename(part, p.filename.c_str());
            if (!p.content_type.empty()) curl_mime_type(part, p.content_type.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (req.method == "POST" || !req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
    }
    if (req.method != "GET" && req.method != "POST")
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    res.status = status;
    // Whether the peer can have seen the request: bytes of it went out, or a connection to it was
    // made (a TLS set-up that then failed counts too, to be safe). A request that never got that
    // far may be sent again whatever it was. (The pre-transfer time is no guide: curl reports one
    // for a refused connection too.)
    long request_bytes = 0;
    curl_off_t connect_us = 0;
    curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE, &request_bytes);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_us);
    res.sent = request_bytes > 0 || connect_us > 0 || status > 0;
    if (rc == CURLE_ABORTED_BY_CALLBACK) res.error = "cancelled";
    else if (rc == CURLE_OPERATION_TIMEDOUT) res.error = "timed out";
    else if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT) res.error = "no connection";
    else if (rc == CURLE_SSL_PINNEDPUBKEYNOTMATCH) res.error = "the device's certificate key has changed";
    else if (rc != CURLE_OK) res.error = curl_easy_strerror(rc);
    char *content_type = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type) == CURLE_OK && content_type) res.content_type = content_type;
    if (req.want_peer_cert) {
        curl_certinfo *info = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_CERTINFO, &info) == CURLE_OK && info && info->num_of_certs > 0)
            for (curl_slist *s = info->certinfo[0]; s; s = s->next)
                if (strncmp(s->data, "Cert:", 5) == 0) res.peer_cert_pem = s->data + 5;
    }

    if (mime) curl_mime_free(mime);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

bool transient(const Response &res) {
    if (res.error == "cancelled") return false;
    if (res.error == "timed out") return !res.sent;   // the connection was never made: no time was spent waiting for an answer
    if (!res.error.empty()) return true;               // no connection, a dropped one, a failed TLS set-up
    const long s = res.status;
    return s == 408 || s == 429 || (s >= 500 && s <= 599 && s != 501 && s != 505);
}

Response send_retrying(Request req, const Retry &retry) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto left_ms = [&] {
        return retry.budget_ms - static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count());
    };
    const long own_ms = req.timeout_ms > 0 ? req.timeout_ms : req.timeout_s * 1000;
    int pause = retry.pause_ms;
    Response res;
    for (int attempt = 1;; ++attempt) {
        req.timeout_ms = std::max(1L, std::min(own_ms, left_ms()));
        res = send(req);
        const bool again = retry.again ? retry.again(res) : !res.ok() && transient(res);
        if (!again || attempt >= retry.attempts || left_ms() - pause < retry.min_ms) return res;
        for (int waited = 0; waited < pause; waited += 50) {
            if ((req.cancel && req.cancel->load()) || (req.cancel_too && req.cancel_too->load())) {
                res.error = "cancelled";
                return res;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        pause *= 2;
        if (retry.before_resend) retry.before_resend();
    }
}

std::string url_encode(const std::string &s) {
    char *esc = curl_easy_escape(nullptr, s.c_str(), static_cast<int>(s.size()));
    std::string out = esc ? esc : "";
    if (esc) curl_free(esc);
    return out;
}

}  // namespace http
