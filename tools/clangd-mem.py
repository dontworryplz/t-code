#!/usr/bin/env python3
"""How much memory clangd takes for this project with given flags or environment.

Starts its own clangd (not t-code's), opens files of the project like micro's lsp plugin does,
waits until clangd is idle (background index done), optionally edits every file a few rounds,
then prints its resident memory next to what clangd says it uses itself ($/memoryUsage). A big
gap between the two is allocator overhead, not clangd's data (see set_malloc_arenas in src/lsp.c).

  tools/clangd-mem.py                               # 25 files, t-code's flags (micro/settings.json)
  tools/clangd-mem.py --files 5 --churn 3
  tools/clangd-mem.py --env GLIBC_TUNABLES=glibc.malloc.arena_max=2
  tools/clangd-mem.py -- --background-index=false   # other clangd flags
"""
import argparse
import glob
import json
import os
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_flags():
    with open(os.path.join(ROOT, "micro", "settings.json")) as f:
        servers = json.load(f)["lsp.server"]
    return servers.split(",")[0].split("=", 1)[1].split()[1:]


class Clangd:
    def __init__(self, flags, env):
        self.proc = subprocess.Popen(["clangd"] + flags, cwd=ROOT, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.lock = threading.Lock()
        self.diagnostics = set()
        self.results = {}
        self.progress = 0
        threading.Thread(target=self.read, daemon=True).start()

    def send(self, message):
        body = json.dumps(message).encode()
        with self.lock:
            self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
            self.proc.stdin.flush()

    def read(self):
        out = self.proc.stdout
        while True:
            headers = {}
            while True:
                line = out.readline()
                if not line:
                    return
                if not line.strip():
                    break
                key, value = line.split(b":", 1)
                headers[key.strip().lower()] = value.strip()
            m = json.loads(out.read(int(headers[b"content-length"])))
            if "method" in m and "id" in m:  # a request: configuration gets one entry per item
                items = m.get("params", {}).get("items", [])
                result = [{}] * len(items) if m["method"] == "workspace/configuration" else None
                self.send({"jsonrpc": "2.0", "id": m["id"], "result": result})
            elif m.get("method") == "textDocument/publishDiagnostics":
                self.diagnostics.add(m["params"]["uri"])
            elif m.get("method") == "$/progress":
                kind = m["params"]["value"].get("kind")
                self.progress += {"begin": 1, "end": -1}.get(kind, 0)
            elif "id" in m:
                self.results[m["id"]] = m.get("result")

    def request(self, id, method, params):
        self.send({"jsonrpc": "2.0", "id": id, "method": method, "params": params})
        while id not in self.results:
            time.sleep(0.05)
        return self.results[id]

    def wait_diagnostics(self, count, timeout=300):
        end = time.time() + timeout
        while len(self.diagnostics) < count and time.time() < end:
            time.sleep(0.2)

    def cpu_ticks(self):
        with open(f"/proc/{self.proc.pid}/stat") as f:
            return sum(int(x) for x in f.read().rsplit(")", 1)[1].split()[11:13])

    def wait_idle(self, timeout=400):
        end, last = time.time() + timeout, self.cpu_ticks()
        while time.time() < end:
            time.sleep(3)
            now = self.cpu_ticks()
            if now - last < 10 and self.progress <= 0:
                return
            last = now

    def memory(self):
        status = {}
        with open(f"/proc/{self.proc.pid}/status") as f:
            for line in f:
                key, _, value = line.partition(":")
                status[key] = value.split()[0] if value.split() else ""
        return int(status["VmRSS"]) // 1024, int(status["VmHWM"]) // 1024


def largest(node, depth, prefix=""):
    out = []
    for key, value in node.items():
        if key.startswith("_"):
            continue
        out.append((value["_total"] >> 20, prefix + key))
        if depth > 1:
            out += largest(value, depth - 1, prefix + key + "/")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--files", type=int, default=25, help="files to open (default 25)")
    parser.add_argument("--churn", type=int, default=0, help="rounds of editing every open file")
    parser.add_argument("--env", action="append", default=[], help="NAME=value for clangd")
    parser.add_argument("flags", nargs="*", help="clangd flags (after --); default: t-code's")
    args = parser.parse_args()

    env = dict(os.environ)
    for pair in args.env:
        name, value = pair.split("=", 1)
        env[name] = value
    files = sorted(glob.glob(f"{ROOT}/src/*.c") + glob.glob(f"{ROOT}/src/*/*.c") + glob.glob(f"{ROOT}/src/*.h"))
    files = files[:args.files]

    clangd = Clangd(args.flags or default_flags(), env)
    clangd.request(1, "initialize", {"processId": os.getpid(), "rootUri": "file://" + ROOT,
                                     "capabilities": {"window": {"workDoneProgress": True}}})
    clangd.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    start = time.time()
    for path in files:
        with open(path) as f:
            text = f.read()
        clangd.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": "file://" + path, "languageId": "c", "version": 1, "text": text}}})
    clangd.wait_diagnostics(len(files))
    opened = time.time() - start
    clangd.wait_idle()
    after_open = clangd.memory()[0]

    for round in range(args.churn):
        clangd.diagnostics.clear()
        for path in files:
            with open(path) as f:
                text = f.read() + "\n" * (round + 1)
            clangd.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": "file://" + path, "version": round + 2},
                "contentChanges": [{"text": text}]}})
        clangd.wait_diagnostics(len(files))
        time.sleep(2)

    usage = clangd.request(2, "$/memoryUsage", {})
    rss, peak = clangd.memory()
    print(f"files {len(files)}, diagnostics for all after {opened:.1f}s")
    print(f"resident {rss} MB (peak {peak} MB) | clangd's own data {usage['_total'] >> 20} MB")
    if args.churn:
        print(f"after opening {after_open} MB, after {args.churn} rounds of edits {rss} MB")
    for mb, name in sorted(largest(usage, 3), reverse=True)[:8]:
        print(f"  {mb:5d} MB  {name}")
    clangd.proc.kill()


if __name__ == "__main__":
    main()
