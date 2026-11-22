"""The network boundary of every test process that runs the app or a mock (ZB-086): what each child gets for an
environment, a trap that every request leaving 127.x must go through and that refuses it, and the probe that a
binary sends every tool's web read to the stand-in.

- child_env(base, keep, allow_origin): no credentials (KEYS) but those KEEP names, no inherited proxy or no-proxy
  setting, and every proxy variable pointing at the trap, with only 127.0.0.1, localhost and ALLOW_ORIGIN's host (the
  AI service of a live run) exempt. Inside the app (experiment builds) ZB_EXP_NET narrows that to exactly 127.x and
  that origin (scheme, host and port), with no redirect followed, and keeps it from identifying the real network;
  ZB_TEST_NETWORK puts it on the stand-in network. A request for anywhere else is refused before it leaves, or
  reaches the trap, which records it and refuses it.
- mock_env(): what a mock (the stand-in web, a scripted model, a simulated device) starts with: a PATH and
  nothing else of the caller's, so no key reaches it.
- local_opener(): urllib without proxies and without following redirects, for a test's own requests to 127.x.
- require_experiments(app): exits unless APP is an experiments build that says it sends every web read to the
  stand-in ("web_all") and refuses hardware; a build without them (web() is the identity there) sends searches
  to the real service.

Standard library only.
"""
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.parse
import urllib.request

KEYS = ("ZB_SEARCH_KEY", "ZB_API_KEY", "ZB_API_BASE")
PROXY_VARS = ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "all_proxy", "ftp_proxy", "FTP_PROXY")
NO_PROXY_VARS = ("no_proxy", "NO_PROXY")
LOCAL_HOSTS = ("127.0.0.1", "localhost")
REQUIRED_CAPS = ("records", "web", "web_all", "no_hardware", "net")


class Trap:
    """A local HTTP server that records the first line of every request (a proxy's CONNECT host:443 included)
    and refuses it with 403: nothing sent to it goes further."""
    def __init__(self):
        seen = self.seen = []
        lock = self.lock = threading.Lock()

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def refuse(self):
                with lock:
                    seen.append(self.requestline)
                self.send_response(403)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
            do_GET = do_POST = do_PUT = do_HEAD = do_CONNECT = do_DELETE = refuse

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def count(self):
        with self.lock:
            return len(self.seen)

    def since(self, n):
        with self.lock:
            return list(self.seen[n:])

    def close(self):
        self.server.shutdown()
        self.server.server_close()


_trap = None
_trap_lock = threading.Lock()


def trap():
    """The process's trap, started on first use."""
    global _trap
    with _trap_lock:
        if _trap is None:
            _trap = Trap()
        return _trap


def origin_of(url):
    """scheme://host:port of URL (the port written out), or "" when it has none."""
    try:
        u = urllib.parse.urlsplit(url or "")
        if u.scheme not in ("http", "https") or not u.hostname or "@" in u.netloc: return ""
        return "%s://%s:%d" % (u.scheme, u.hostname.lower(), u.port or (443 if u.scheme == "https" else 80))
    except ValueError:
        return ""


def send_to_trap(url, target):
    """A request for TARGET (an absolute URL) written straight to the trap's socket at URL, as a proxy is sent one:
    no proxy setting, no NO_PROXY and no name lookup is involved, so nothing can leave this computer. Returns the
    trap's status line."""
    import socket
    u = urllib.parse.urlsplit(url)
    with socket.create_connection((u.hostname, u.port), timeout=5) as s:
        host = urllib.parse.urlsplit(target).hostname or ""
        s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % (target, host)).encode())
        return s.recv(200).split(b"\r\n", 1)[0].decode("latin-1", "replace")


def child_env(base=None, keep=(), allow_origin="", **extra):
    """BASE (default os.environ) without credentials but KEEP, without inherited proxy settings, with every proxy
    variable at the trap and only 127.0.0.1, localhost and ALLOW_ORIGIN's host exempt; the app's own pin (ZB_EXP_NET:
    loopback, or loopback and exactly ALLOW_ORIGIN) and the stand-in network (ZB_TEST_NETWORK); then EXTRA (None
    removes)."""
    origin = origin_of(allow_origin) if allow_origin else ""
    allow_hosts = (host_of(origin),) if origin else ()
    env = dict(os.environ if base is None else base)
    for k in KEYS:
        if k not in keep:
            env.pop(k, None)
    for k in PROXY_VARS + NO_PROXY_VARS:
        env.pop(k, None)
    url = trap().url
    for k in PROXY_VARS:
        env[k] = url
    exempt = ",".join(LOCAL_HOSTS + tuple(h for h in allow_hosts if h))
    for k in NO_PROXY_VARS:
        env[k] = exempt
    env["ZB_EXP_NET"] = origin or "loopback"   # inside the app: nothing but 127.x (and the AI origin) goes out
    env["ZB_TEST_NETWORK"] = "1"               # the stand-in network: the real one is never identified
    for k, v in extra.items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = v
    return env


def sanitize():
    """This process's own environment made what child_env gives, so that every child it starts, with os.environ or a
    copy of it, inherits no key and no proxy of the caller's, is pinned (ZB_EXP_NET) and is on the stand-in network.
    For the standalone end-to-end scripts, which start the app and mocks in many places."""
    clean = child_env()
    for k in list(os.environ):
        if k not in clean:
            del os.environ[k]
    os.environ.update(clean)


def mock_env(**extra):
    """A mock's environment: a PATH, a language and no other variable of the caller's."""
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8", "PYTHONDONTWRITEBYTECODE": "1"}
    env.update(extra)
    return env


def host_of(url):
    try:
        return urllib.parse.urlsplit(url or "").hostname or ""
    except ValueError:
        return ""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None   # a 3xx comes back as an HTTPError: reported, never followed


def local_opener():
    """urllib for a test's own requests to 127.x: no proxy, no redirect followed."""
    return urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())


def no_redirect_opener():
    """urllib that follows no redirect (it uses the environment's proxies, if any)."""
    return urllib.request.build_opener(_NoRedirect())


def caps(app):
    """The capabilities APP's records say it has (ZB_EVAL_RECORDS), or None when it writes none."""
    folder = tempfile.mkdtemp(prefix="zb-caps-")
    try:
        path = os.path.join(folder, "records.jsonl")
        env = child_env(mock_env(), HOME=folder, XDG_CONFIG_HOME=folder + "/c", XDG_DATA_HOME=folder + "/d",
                        XDG_CACHE_HOME=folder + "/k", ZB_EVAL_RECORDS=path, ZB_DRY_RUN="1", ZB_NO_AUDIO="1",
                        ZB_EXP_NO_HARDWARE="1", ZB_TEST_NETWORK="1")
        try:
            subprocess.run([app, "--chat"], input="", env=env, capture_output=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            return None
        try:
            with open(path, encoding="utf-8") as f:
                first = json.loads(f.readline())
        except (OSError, ValueError):
            return None
        return first.get("caps") if isinstance(first, dict) and isinstance(first.get("caps"), list) else None
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def missing_caps(app):
    """What APP lacks of REQUIRED_CAPS (all of them when it writes no records)."""
    got = caps(app)
    return list(REQUIRED_CAPS) if got is None else [c for c in REQUIRED_CAPS if c not in got]


def require_experiments(app):
    """Exit unless APP is an experiments build that sends every web read to the stand-in and refuses hardware."""
    if not os.access(app, os.X_OK):
        sys.exit("no app at %s (build the cli preset first)" % app)
    lacking = missing_caps(app)
    if lacking:
        sys.exit("refused: %s is not an experiments build that keeps tests off the real web (it lacks %s); build the "
                 "cli preset of this checkout" % (app, ", ".join(lacking)))


# ---------------------------------------------------------------- its own test (tests/run.py's runner group)
FAKE_APP = r'''#!/usr/bin/env python3
import json, os, sys
with open(os.environ.get("FAKE_LOG") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "calls.log"), "a") as f:
    f.write(json.dumps({"argv": sys.argv[1:], "probe": bool(os.environ.get("ZB_EVAL_RECORDS"))}) + "\n")
path = os.environ.get("ZB_EVAL_RECORDS")
if path:
    with open(path, "w") as f:
        f.write(json.dumps({"v": 1, "ev": "start", "caps": %s}) + "\n")
        f.write(json.dumps({"ev": "end"}) + "\n")
print("{}")
'''


def self_test():
    n, failed = 0, []

    def check(what, ok, got=None):
        nonlocal n
        n += 1
        if not ok:
            failed.append(what + ("" if got is None else ": %r" % (got,)))
    base = {"PATH": "/bin", "ZB_SEARCH_KEY": "s", "ZB_API_KEY": "k", "ZB_API_BASE": "https://ai.example/v1", "NO_PROXY": "*",
            "no_proxy": "*", "https_proxy": "http://corp.example:3128", "OTHER": "1"}
    env = child_env(base)
    check("child_env: no key, no AI service", not any(k in env for k in KEYS), env)
    check("child_env: every proxy variable at the trap", all(env[k] == trap().url for k in PROXY_VARS), env)
    check("child_env: only loopback exempt", env["NO_PROXY"] == env["no_proxy"] == "127.0.0.1,localhost", env)
    check("child_env: the app pinned to loopback, on the stand-in network", env["ZB_EXP_NET"] == "loopback" and env["ZB_TEST_NETWORK"] == "1")
    env = child_env(base, keep=("ZB_API_KEY", "ZB_API_BASE"), allow_origin="https://ai.example/v1")
    check("child_env: kept keys and one exempt host", env.get("ZB_API_KEY") == "k" and "ZB_SEARCH_KEY" not in env and
          env["NO_PROXY"] == "127.0.0.1,localhost,ai.example", env)
    check("child_env: the app pinned to exactly the AI origin", env["ZB_EXP_NET"] == "https://ai.example:443", env)
    old = {k: os.environ.get(k) for k in ("ZB_SEARCH_KEY", "ZB_API_KEY")}
    os.environ.update(ZB_SEARCH_KEY="s", ZB_API_KEY="k")
    try:
        check("mock_env: nothing of the caller's but PATH", set(mock_env()) == {"PATH", "LANG", "PYTHONDONTWRITEBYTECODE"}, mock_env())
    finally:
        for k, v in old.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
    # Every entry point that runs the cli refuses a build without the experiments before it runs anything else:
    # the fake app logs each call; only the probe may have been made.
    here = os.path.dirname(os.path.abspath(__file__))
    folder = tempfile.mkdtemp(prefix="zb-netguard-")
    try:
        for name, caps_ in (("old", '["records", "web", "no_hardware"]'), ("none", '[]'),
                            ("good", '["records", "web", "web_all", "no_hardware", "net"]'), ("nonet", '["records", "web", "web_all", "no_hardware"]')):
            app = os.path.join(folder, name)
            with open(app, "w") as f:
                f.write(FAKE_APP % caps_)
            os.chmod(app, 0o755)
        check("probe: a build without web_all lacks it", missing_caps(os.path.join(folder, "old")) == ["web_all", "net"])
        check("probe: a build without the network pin (round 2) lacks it", missing_caps(os.path.join(folder, "nonet")) == ["net"])
        check("probe: a build with all of them lacks nothing", missing_caps(os.path.join(folder, "good")) == [])
        for script in ("sim/tools_cli.py", "sim/agent_loop_test.py", "sim/home_chat_e2e.py", "sim/selfheal_chat_e2e.py"):
            log = os.path.join(folder, "calls.log")   # the fake app's own (the probe gives it no environment of ours)
            if os.path.exists(log):
                os.remove(log)
            p = subprocess.run([sys.executable, "-B", os.path.join(here, script), os.path.join(folder, "old")], capture_output=True,
                               text=True, timeout=60)
            calls = [json.loads(line) for line in open(log)] if os.path.exists(log) else []
            check("%s refuses a build without the experiments, having run nothing but the probe" % script,
                  p.returncode != 0 and "refused" in p.stdout + p.stderr and calls and all(c["probe"] for c in calls),
                  (p.returncode, (p.stdout + p.stderr)[-200:], calls))
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    # Every script that starts the app or a mock builds their environments here (round 2, W4): the standalone
    # device tests sanitize their own environment first, and every mock they start gets mock_env.
    import re
    for script in ("sim/tapo_e2e.py", "sim/kasa_e2e.py", "sim/wiz_e2e.py", "sim/shelly_e2e.py", "sim/hue_e2e.py", "sim/homekit_e2e.py",
                   "sim/firetv_e2e.py", "sim/home_chat_e2e.py", "sim/selfheal_chat_e2e.py", "sim/agent_loop_test.py",
                   "sim/tools_cli.py", "tools/web_contract.py"):
        with open(os.path.join(here, script), encoding="utf-8") as f:
            text = f.read()
        popens = [m.start() for m in re.finditer(r"subprocess\.Popen\(", text)]
        bare = []
        for i in popens:
            depth, k = 0, text.index("(", i)
            while True:
                depth += {"(": 1, ")": -1}.get(text[k], 0)
                k += 1
                if depth == 0: break
            call = text[i:k]
            if ("mock" in call.lower() or "fake_" in call) and "netguard.mock_env()" not in call:
                bare.append(call[:80])
        check("%s starts every mock with mock_env" % script, not bare, bare)
        if script.endswith("_e2e.py") and script not in ("sim/home_chat_e2e.py", "sim/selfheal_chat_e2e.py"):
            check("%s sanitizes its own environment first" % script, "netguard.sanitize()" in text)
    # local_opener reaches 127.x whatever proxy the environment names; the trap sees nothing.
    class Quiet(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            self.send_response(204)
            self.end_headers()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    old = {k: os.environ.get(k) for k in ("http_proxy", "no_proxy", "NO_PROXY")}
    os.environ["http_proxy"] = trap().url
    os.environ.pop("no_proxy", None)
    os.environ.pop("NO_PROXY", None)
    before = trap().count()
    try:
        try:
            local_opener().open("http://127.0.0.1:%d/nothing-here" % srv.server_address[1], timeout=5)
        except Exception:   # noqa: BLE001  a 404 from the local server is fine
            pass
        check("local_opener takes no proxy", trap().since(before) == [], trap().since(before))
    finally:
        srv.shutdown()
        srv.server_close()
        for k, v in old.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
    for f in failed:
        print("FAIL  " + f)
    print("netguard self-test: %d checks, %d failed" % (n, len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv[1:] else 2)
