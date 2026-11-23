// The http::Request options smart-home drivers use, against the local peer in http_peer.py:
// no_redirect, timeout_ms, digest credentials, insecure, a pinned public key, the peer's
// certificate, a client certificate, and the defaults of a request to a device (no redirects to
// elsewhere, no proxy); reply headers kept by name (keep_headers); and send_retrying, which sends a
// read again only after a failure on the way, inside its time budget. Started by http_peer.py, which passes:
//   http_test HTTP_PORT HTTPS_PORT HTTPS_CLIENT_PORT PIN CLIENT_CERT_FILE CLIENT_KEY_FILE PROXY_PORT
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "core/util.h"
#include "net/http.h"

namespace {

int g_checks = 0, g_failed = 0;

void check(bool ok, const char *what, const std::string &detail) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    printf("FAIL  %s  -> %s\n", what, detail.c_str());
}

std::string seen(const http::Response &r) { return "status " + std::to_string(r.status) + ", error '" + r.error + "', body '" + r.body.substr(0, 80) + "'"; }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "usage: http_test HTTP_PORT HTTPS_PORT HTTPS_CLIENT_PORT PIN CERT KEY PROXY_PORT\n"); return 2; }
    const std::string plain = std::string("http://127.0.0.1:") + argv[1], tls = std::string("https://127.0.0.1:") + argv[2],
                      tls_client = std::string("https://127.0.0.1:") + argv[3], pin = argv[4],
                      proxy = std::string("http://127.0.0.1:") + argv[7];
    // Whatever proxy the shell running the tests names is not wanted here.
    for (const char *v : {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY"})
        unsetenv(v);
    std::string cert, key;
    if (!util::read_file(argv[5], cert) || !util::read_file(argv[6], key)) { fprintf(stderr, "cannot read the client certificate\n"); return 2; }
    http::global_init();

    http::Request r;
    r.url = plain + "/redirect";
    http::Response res = http::send(r);
    check(res.status == 200 && res.body == "target", "a redirect is followed by default", seen(res));
    r.no_redirect = true;
    res = http::send(r);
    check(res.status == 302 && res.body != "target", "no_redirect returns the redirect itself", seen(res));

    r = http::Request();
    r.url = plain + "/slow";
    r.timeout_ms = 300;
    const auto t0 = std::chrono::steady_clock::now();
    res = http::send(r);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(res.error == "timed out" && secs < 1.5, "timeout_ms cuts a slow answer short", seen(res) + ", " + std::to_string(secs) + " s");

    r = http::Request();
    r.url = plain + "/digest";
    res = http::send(r);
    check(res.status == 401 && !res.digest_answered, "the digest page refuses a request without credentials", seen(res));
    r.digest_user = "user";
    r.digest_pass = "pass word";
    res = http::send(r);
    check(res.status == 200 && res.body == "digest ok" && res.digest_answered, "digest credentials are accepted", seen(res));
    r.digest_pass = "wrong";
    res = http::send(r);
    check(res.status == 401 && res.digest_answered, "wrong digest credentials are refused", seen(res));
    // A 401 whose body is cut short keeps its status (a device's refusal is one whatever came with it), and
    // says whether it answered the login or was the challenge before it, which refused nothing.
    r.url = plain + "/digest-cut";
    res = http::send(r);
    check(res.status == 401 && !res.error.empty() && res.digest_answered, "a refusal of the login cut short is still a 401 to it",
          seen(res));
    r.url = plain + "/challenge-cut";
    res = http::send(r);
    check(res.status == 401 && !res.error.empty() && !res.digest_answered, "a challenge cut short answered no login", seen(res));

    r = http::Request();
    r.url = tls + "/hello";
    r.timeout_ms = 3000;
    res = http::send(r);
    check(!res.error.empty(), "a self-signed certificate is refused by default", seen(res));
    r.insecure = true;
    res = http::send(r);
    check(res.status == 200 && res.body == "tls ok", "insecure accepts the device's own certificate", seen(res));
    r.pinned_pubkey = pin;
    res = http::send(r);
    check(res.status == 200, "the right pinned key is accepted", seen(res));
    r.pinned_pubkey = "sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    res = http::send(r);
    check(res.error == "the device's certificate key has changed", "a different key is refused, even when insecure", seen(res));
    r.pinned_pubkey.clear();
    r.want_peer_cert = true;
    res = http::send(r);
    check(res.peer_cert_pem.find("-----BEGIN CERTIFICATE-----") != std::string::npos, "the peer's certificate is handed back",
          seen(res));

    r = http::Request();
    r.url = tls_client + "/who";
    r.insecure = true;
    r.timeout_ms = 3000;
    res = http::send(r);
    check(!res.ok(), "the peer that wants a client certificate refuses a request without one", seen(res));
    r.client_cert_pem = cert;
    r.client_key_pem = key;
    res = http::send(r);
    check(res.status == 200 && res.body == "client ok", "a client certificate given as PEM text is presented", seen(res));

    // A request to a device does not follow a redirect, here to another host: a device's key in a
    // header must never travel anywhere else. The device-only options imply it.
    auto peer_saw = [&] {
        http::Request q;
        q.local = true;
        q.url = plain + "/seen";
        return http::send(q).body;
    };
    r = http::Request();
    r.url = plain + "/redirect-away";
    r.headers = {"hue-application-key: SECRET-DEVICE-KEY"};
    r.local = true;
    res = http::send(r);
    check(res.status == 307 && peer_saw() == "away=0 proxy=0", "a device request returns a redirect instead of following it",
          seen(res) + ", " + peer_saw());
    r.local = false;
    r.digest_user = "user";
    r.digest_pass = "pass word";
    res = http::send(r);
    check(res.status == 307 && peer_saw() == "away=0 proxy=0", "digest credentials make it a device request", seen(res) + ", " + peer_saw());
    r = http::Request();
    r.url = tls + "/redirect-away";
    r.headers = {"hue-application-key: SECRET-DEVICE-KEY"};
    r.insecure = true;
    r.timeout_ms = 3000;
    res = http::send(r);
    check(res.status == 307 && peer_saw() == "away=0 proxy=0", "so does accepting a device's own certificate", seen(res) + ", " + peer_saw());

    // A proxy named in the environment is for the internet: a request to a device goes straight to it.
    setenv("http_proxy", proxy.c_str(), 1);
    setenv("https_proxy", proxy.c_str(), 1);
    r = http::Request();
    r.url = plain + "/hello";
    r.timeout_ms = 3000;
    res = http::send(r);
    check(res.status == 502, "(another request does go through the pretend proxy)", seen(res));
    r.local = true;
    res = http::send(r);
    check(res.status == 200 && res.body == "tls ok", "a device request does not go through a proxy", seen(res));
    r = http::Request();
    r.url = tls + "/hello";
    r.insecure = true;
    r.timeout_ms = 3000;
    res = http::send(r);
    check(res.status == 200 && res.body == "tls ok", "nor does one with a device's own certificate", seen(res));
    unsetenv("http_proxy");
    unsetenv("https_proxy");
    check(peer_saw() == "away=0 proxy=1", "the proxy saw only the internet request, the other host nothing", peer_saw());

    // ---- reply headers kept by name (a search service's allowance left): only those asked for, in any case,
    // and only the last answer's
    auto kept = [](const http::Response &res) {
        std::string out;
        for (const auto &h : res.headers) out += h.first + "=" + h.second + ";";
        return out;
    };
    r = http::Request();
    r.url = plain + "/headers";
    res = http::send(r);
    check(res.status == 200 && res.headers.empty(), "no reply header is kept unless asked for", kept(res));
    r.keep_headers = {"X-RATELIMIT-REMAINING", "x-missing"};
    res = http::send(r);
    check(kept(res) == "x-ratelimit-remaining=42;", "an asked-for header is kept by its lower-case name, trimmed", kept(res));
    r.url = plain + "/headers-moved";
    res = http::send(r);
    check(res.status == 200 && res.body == "target" && res.headers.empty(), "a redirect's headers are not the answer's",
          seen(res) + ", " + kept(res));

    // ---- send_retrying: only failures on the way are sent again, inside the time given
    auto tries = [&](const std::string &key) {
        http::Request q;
        q.url = plain + "/tries/" + key;
        q.timeout_ms = 3000;
        return http::send(q).body;
    };
    auto retrying = [&](const std::string &path, http::Retry retry) {
        http::Request q;
        q.url = plain + path;
        q.timeout_ms = 3000;
        return http::send_retrying(q, retry);
    };
    http::Retry quick;
    quick.pause_ms = 50;
    quick.min_ms = 100;
    int resends = 0;
    quick.before_resend = [&] { ++resends; };
    res = retrying("/flaky/a/1", quick);
    check(res.status == 200 && res.body == "fine" && tries("a") == "2" && resends == 1, "a 503 is sent once more, and then works",
          seen(res) + ", tries " + tries("a"));
    res = retrying("/flaky/b/5", quick);
    check(res.status == 503 && tries("b") == "2", "at most `attempts` sends in all", seen(res) + ", tries " + tries("b"));
    res = retrying("/status/404/c", quick);
    check(res.status == 404 && tries("c") == "1", "a 404 is not sent again", seen(res) + ", tries " + tries("c"));
    res = retrying("/status/400/c2", quick);
    check(res.status == 400 && tries("c2") == "1", "nor a 400", seen(res) + ", tries " + tries("c2"));
    res = retrying("/status/429/d", quick);
    check(res.status == 429 && tries("d") == "2", "a 429 is sent again", seen(res) + ", tries " + tries("d"));
    res = retrying("/status/501/d2", quick);
    check(res.status == 501 && tries("d2") == "1", "a 501 (never going to work) is not", seen(res) + ", tries " + tries("d2"));
    res = retrying("/hangup/e", quick);
    check(!res.error.empty() && res.sent && tries("e") == "2", "a connection closed without an answer is sent again", seen(res) + ", tries " + tries("e"));
    {
        http::Request q;
        q.url = plain + "/slowcount/f";
        q.timeout_ms = 300;
        res = http::send_retrying(q, quick);
        check(res.error == "timed out" && res.sent && tries("f") == "1", "a timeout once the request went out is not sent again",
              seen(res) + ", tries " + tries("f"));
    }
    {
        http::Request q;
        q.url = "http://127.0.0.1:1/nothing-listens";
        q.timeout_ms = 2000;
        res = http::send(q);
        check(res.error == "no connection" && !res.sent && http::transient(res), "a refused connection: not sent, and worth another try",
              seen(res));
    }
    {
        http::Retry slow = quick;
        slow.attempts = 5;
        slow.pause_ms = 500;
        slow.budget_ms = 1500;
        slow.min_ms = 300;
        const auto start = std::chrono::steady_clock::now();
        res = retrying("/flaky/g/9", slow);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(res.status == 503 && tries("g") == "2" && s < 1.5, "no send starts once the budget is nearly spent",
              seen(res) + ", tries " + tries("g") + ", " + std::to_string(s) + " s");
    }
    {
        std::atomic<bool> stop{false};
        http::Request q;
        q.url = plain + "/flaky/h/9";
        q.timeout_ms = 3000;
        q.cancel = &stop;
        http::Retry patient = quick;
        patient.attempts = 3;
        patient.pause_ms = 3000;
        patient.budget_ms = 20000;
        std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); stop = true; });
        const auto start = std::chrono::steady_clock::now();
        res = http::send_retrying(q, patient);
        stopper.join();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(res.error == "cancelled" && s < 1.0 && tries("h") == "1", "cancelling ends the pause at once", seen(res) + ", " + std::to_string(s) + " s");
    }
    {
        http::Response t;
        t.status = 500;
        bool all = http::transient(t);
        t.status = 408; all = all && http::transient(t);
        t.status = 505; all = all && !http::transient(t);
        t.status = 0; t.error = "timed out"; t.sent = false; all = all && http::transient(t);
        t.sent = true; all = all && !http::transient(t);
        t.error = "cancelled"; t.sent = false; all = all && !http::transient(t);
        check(all, "transient(): 500 and 408 yes, 505 no, a timeout only before the request went out, a cancel never", "");
    }

    printf("%s  http options (%d checks)\n", g_failed ? "FAIL" : "ok  ", g_checks);
    return g_failed ? 1 : 0;
}
