#include "net/http.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

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
    return (ctx->req->cancel && ctx->req->cancel->load()) ? 1 : 0;
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
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, req.connect_timeout_s);
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
    if (rc == CURLE_ABORTED_BY_CALLBACK) res.error = "cancelled";
    else if (rc == CURLE_OPERATION_TIMEDOUT) res.error = "timed out";
    else if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT) res.error = "no connection";
    else if (rc != CURLE_OK) res.error = curl_easy_strerror(rc);

    if (mime) curl_mime_free(mime);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

std::string url_encode(const std::string &s) {
    char *esc = curl_easy_escape(nullptr, s.c_str(), static_cast<int>(s.size()));
    std::string out = esc ? esc : "";
    if (esc) curl_free(esc);
    return out;
}

}  // namespace http
