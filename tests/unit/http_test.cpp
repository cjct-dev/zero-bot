// The http::Request options smart-home drivers use, against the local peer in http_peer.py:
// no_redirect, timeout_ms, digest credentials, insecure, a pinned public key, the peer's
// certificate, a client certificate, and the defaults of a request to a device (no redirects to
// elsewhere, no proxy). Started by http_peer.py, which passes:
//   http_test HTTP_PORT HTTPS_PORT HTTPS_CLIENT_PORT PIN CLIENT_CERT_FILE CLIENT_KEY_FILE PROXY_PORT
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    check(res.status == 401, "the digest page refuses a request without credentials", seen(res));
    r.digest_user = "user";
    r.digest_pass = "pass word";
    res = http::send(r);
    check(res.status == 200 && res.body == "digest ok", "digest credentials are accepted", seen(res));
    r.digest_pass = "wrong";
    res = http::send(r);
    check(res.status == 401, "wrong digest credentials are refused", seen(res));

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

    printf("%s  http options (%d checks)\n", g_failed ? "FAIL" : "ok  ", g_checks);
    return g_failed ? 1 : 0;
}
