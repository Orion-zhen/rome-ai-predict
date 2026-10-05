#!/usr/bin/env python3
"""共享客户端的本地 HTTP 协议和生命周期测试，不连接输入法或模型服务。"""
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary, scenario = sys.argv[1:]
requests = []
errors = []
started = threading.Event()
release = threading.Event()


def token(text, score):
    return {"token": text, "bytes": list(text.encode()), "logprob": score}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(request)
        expected = {"model", "prompt", "n", "max_tokens", "logprobs", "temperature", "stream", "echo"}
        if self.path != "/v1/completions" or set(request) != expected:
            errors.append("wrong request shape or endpoint")
        for name, value in {"model": "shared-test-model", "n": 1, "max_tokens": 1,
                            "logprobs": 3, "temperature": 0.7, "stream": False, "echo": False}.items():
            if request.get(name) != value:
                errors.append(f"wrong request field: {name}")
        if request["prompt"] not in {"今天😀一起", "second", "latest"}:
            errors.append("prompt changed")
        auth = "Bearer shared-test-key" if scenario == "auth" else None
        if self.headers.get("Authorization") != auth:
            errors.append("wrong authentication")
        started.set()
        if scenario.startswith("active-") and request["prompt"] == "今天😀一起":
            release.wait(5)
        elif scenario == "timeout":
            release.wait(0.4)
        top = [token("看", -1.1), token("去", -2.1), token("吃", -0.1)]
        position = {"top_logprobs": top}
        response = {"choices": [{"text": "不是候选", "logprobs": {"content": [position]}}]}
        status = 200
        if scenario == "standard":
            response["choices"][0]["logprobs"] = {"top_logprobs": [{"去": -2.1, "吃": -0.1, "看": -1.1}]}
        elif scenario == "bytes":
            top[0] = {"token": "▁tomorrow", "bytes": list(b" tomorrow"), "logprob": -1.1}
        elif scenario == "dedup":
            position["top_logprobs"] = [token("吃", -0.1), token("吃", -0.2), token("看", -0.3)]
        elif scenario == "filter":
            position["top_logprobs"] = [
                {"token": "incomplete", "bytes": [229], "logprob": -0.1},
                token("看\n说明", -0.2), token("吃", -0.3), token("不补位", -0.4)]
        elif scenario == "utf8":
            position["top_logprobs"] = [
                {"token": "overlong", "bytes": [0xc0, 0xaf], "logprob": -0.1},
                {"token": "surrogate", "bytes": [0xed, 0xa0, 0x80], "logprob": -0.2},
                token("😀", -0.3)]
        elif scenario == "length":
            position["top_logprobs"] = [token("😀" * 128, -0.1), token("中" * 129, -0.2), token(" 吃", -0.3)]
        elif scenario == "special":
            position["top_logprobs"] = [
                {"token": "<eos>", "bytes": None, "logprob": -0.1},
                token("   ", -0.2), token("吃", -0.3)]
        elif scenario == "first-position":
            response["choices"][0]["logprobs"]["content"].append({"top_logprobs": [token("错", 0.0)]})
            response["choices"].append({"logprobs": {"content": [{"top_logprobs": [token("错", 0.0)]}]}})
        elif scenario == "empty":
            response["choices"] = []
        elif scenario == "shape":
            response = {"choices": [{"message": {"content": "private-server-body"}}]}
        elif scenario == "score":
            top[0]["logprob"] = "private-server-body"
        elif scenario == "invalid-bytes":
            top[0]["bytes"] = [256]
        elif scenario == "missing-bytes":
            del top[0]["bytes"]
        elif scenario == "http-error":
            status, response = 503, {"error": "private-server-body"}
        elif scenario == "redirect":
            status = 302
        data = json.dumps(response, ensure_ascii=False).encode()
        if scenario == "json":
            data = b"private-server-body"
        elif scenario == "duplicate-key":
            data = b'{"choices": [], "choices": []}'
        elif scenario == "trailing-json":
            data += b' {"extra": 1}'
        elif scenario == "big":
            data = b"x" * (1024 * 1024 + 1)
        try:
            self.send_response(status)
            if scenario == "redirect":
                self.send_header("Location", "/v1/redirect-target")
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            # 取消、超时和响应大小限制要求客户端提前关闭连接。
            pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
process = None
try:
    env = {**os.environ, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
    process = subprocess.Popen([binary, scenario, f"http://127.0.0.1:{server.server_port}/v1"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, env=env)
    if scenario.startswith("active-"):
        assert process.stdout.readline().strip() == "ready"
        assert started.wait(5), "HTTP request never started"
        process.stdin.write("\n")
        process.stdin.flush()
    output, diagnostic = process.communicate(timeout=8)
    assert process.returncode == 0, diagnostic
    assert not errors, errors
    assert "shared-test-key" not in output + diagnostic
    assert "private-server-body" not in output + diagnostic
    results = json.loads(output)["results"]
    if scenario in {"queued-cancel", "queued-destroyed", "active-shutdown"}:
        assert results == [], results
    else:
        assert len(results) == 1, results
        result = results[0]
        generation = (3 if scenario == "active-replaced" else
                      2 if scenario in {"queued-replaced", "active-cancel"} else 1)
        assert result["generation"] == generation, results
        failures = {"json", "duplicate-key", "trailing-json", "shape", "score", "invalid-bytes",
                    "missing-bytes", "http-error", "redirect", "big", "timeout"}
        if scenario in failures:
            assert result.get("error") and "tokens" not in result, result
        else:
            expected = {
                "bytes": ["吃", " tomorrow", "去"], "dedup": ["吃", "看"], "filter": ["吃"],
                "utf8": ["😀"], "length": ["😀" * 128, " 吃"], "special": ["吃"], "empty": [],
            }.get(scenario, ["吃", "看", "去"])
            assert result["tokens"] == expected, result
    if scenario == "redirect":
        assert len(requests) == 1, "client followed HTTP redirect"
    print(f"PASS: shared {scenario}")
finally:
    if process is not None and process.poll() is None:
        process.kill()
        process.communicate()
    release.set()
    server.shutdown()
    server.server_close()
    thread.join()
