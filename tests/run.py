#!/usr/bin/env python3
"""隔离 XDG + 原版 Fcitx5-Rime + 本地 Completions HTTP 服务器的集成测试。"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary, work, case, addon_data, addon_library, probe, defaults = sys.argv[1:]
work = Path(work).resolve()
addon_data = Path(addon_data).resolve()
addon_library = Path(addon_library).resolve()
system_addons = subprocess.check_output([binary, "--system-addon-dir"], text=True).strip()
requests = []
errors = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(body)
        expected = {"model", "prompt", "n", "max_tokens", "logprobs", "temperature", "stream", "echo"}
        if self.path != "/v1/completions" or set(body) != expected:
            errors.append("wrong endpoint or request fields")
        if body["model"] != "local-test-model" or body["n"] != 1 or body["logprobs"] != 3:
            errors.append("configuration did not reach request")
        if body["stream"] is not False or body["echo"] is not False:
            errors.append("expected non-streaming continuation without echo")
        if body["max_tokens"] != 1 or body["temperature"] != 0.7:
            errors.append("generation parameters did not reach request")
        auth = "Bearer local-test-key" if case == "api-auth" else None
        if self.headers.get("Authorization") != auth:
            errors.append("unexpected authorization header")
        if len(body["prompt"]) > 256 or "尾巴" in body["prompt"]:
            errors.append("invalid context slicing")
        if case == "unicode" and len(body["prompt"]) != 256:
            errors.append("Unicode context window was not capped")
        delay = 1.5 if case in {"api-timeout", "shutdown"} else 0.25
        if case == "new-request" and body["prompt"].endswith("一起"):
            delay = 0.9
        time.sleep(delay)
        tokens = (["饭", "面", "菜"] if body["prompt"].endswith("吃") else ["吃", "看", "去"])
        if body["prompt"].endswith("你好"):
            tokens = ["呀", "！", "，"]
        if case == "api-space":
            tokens = [" tomorrow", " afternoon", " evening"]

        def entry(text, logprob):
            return {"token": text, "bytes": list(text.encode()), "logprob": logprob}

        # 故意打乱概率顺序，且使采样文本与概率最高 token 不同。
        top = [entry(tokens[i], -0.1 - i) for i in (1, 2, 0)]
        if case == "api-space":
            for item in top:
                item["token"] = item["token"].replace(" ", "▁")
        position = {"token": "采样", "bytes": list("采样".encode()), "logprob": -4.0,
                    "top_logprobs": top}
        status = 200
        response = {"choices": [{"text": "不是候选", "logprobs": {"content": [position]}}]}
        if case == "api-error":
            status, response = 503, {"error": "test unavailable"}
        elif case == "api-shape":
            response = {"choices": [{"message": {"content": "not a completion"}}]}
        elif case == "api-empty":
            response["choices"][0]["logprobs"]["content"] = []
        elif case == "api-dedup":
            position["top_logprobs"] = [entry("吃", -0.1), entry("吃", -0.2), entry("看", -0.3)]
        elif case == "api-standard":
            response["choices"][0]["logprobs"] = {
                "top_logprobs": [{"看": -1.0, "去": -2.0, "吃": -0.1}, {"错误位置": 0.0}]}
        elif case == "api-first-position":
            response["choices"][0]["logprobs"]["content"].append(
                {"top_logprobs": [entry("错误位置", 0.0)]})
        elif case == "api-bytes":
            position["top_logprobs"] = [
                {"token": "bytes: E5", "bytes": [229], "logprob": -0.1},
                entry("看\n说明", -0.2), entry("吃", -0.3), entry("不能补位", -0.4)]
        elif case == "api-special":
            position["top_logprobs"] = [
                {"token": "<eos>", "bytes": None, "logprob": -0.1},
                entry("吃", -0.2), entry(" ", -0.3)]
        elif case == "api-score":
            top[0]["logprob"] = "invalid"
        elif case == "api-invalid-bytes":
            top[0]["bytes"] = [256]
        data = json.dumps(response, ensure_ascii=False).encode()
        if case == "api-json":
            data = b"not json"
        elif case == "api-big":
            data = b"x" * (1024 * 1024 + 1)
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            # 测试要求客户端取消或超时后关闭连接。
            pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix=f"test-{case}-", dir=work) as directory:
        root = Path(directory)
        env = os.environ.copy()
        for variable, subdir in {
            "HOME": "home", "XDG_CONFIG_HOME": "config", "XDG_DATA_HOME": "data",
            "XDG_CACHE_HOME": "cache", "XDG_STATE_HOME": "state",
            "XDG_RUNTIME_DIR": "runtime", "TMPDIR": "tmp",
        }.items():
            path = root / subdir
            path.mkdir(mode=0o700)
            env[variable] = str(path)
        global_data = root / "global-data"
        env["XDG_DATA_DIRS"] = f"{global_data}:{addon_data}:/usr/local/share:/usr/share"
        env["XDG_CONFIG_DIRS"] = str(root / "config")
        env["FCITX_ADDON_DIRS"] = os.pathsep.join((str(addon_library), system_addons))
        # Fcitx 专用变量优先于 XDG，清除它们以验证非默认 XDG 路径。
        for name in ("FCITX_DATA_HOME", "FCITX_CONFIG_HOME", "FCITX_DATA_DIRS",
                     "FCITX_CONFIG_DIRS", "SKIP_FCITX_PATH", "SKIP_FCITX_SYSTEM_PATH",
                     "SKIP_FCITX_USER_PATH"):
            env.pop(name, None)
        env["NO_PROXY"] = "127.0.0.1"
        env["no_proxy"] = "127.0.0.1"
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(name, None)
        rime = root / "data/fcitx5/rime"
        rime.mkdir(parents=True)
        config = root / "config/fcitx5/conf/rome-ai-predict.yaml"
        config.parent.mkdir(parents=True)
        default_text = Path(defaults).read_text(encoding="utf-8")
        if case != "missing-config":
            global_config = global_data / "fcitx5/conf/rome-ai-predict.yaml"
            global_config.parent.mkdir(parents=True)
            settings = {
                "enabled": True,
                "base_url": f"http://127.0.0.1:{server.server_port}/v1/",
                "model": "local-test-model",
                "candidates": 3,
                "context_chars": 256,
            }
            if case == "partial-config":
                settings.update(api_key="global-key-must-be-cleared", candidates=1,
                                temperature=1.3, context_chars=2)
            text = default_text
            for key, value in settings.items():
                text = re.sub(rf"^{key}:.*$", f"{key}: {json.dumps(value)}",
                              text, flags=re.MULTILINE)
            global_config.write_text("[]\n" if case == "invalid-global" else text,
                                     encoding="utf-8")
        if case not in {"missing-config", "global-config"}:
            settings = {
                "enabled": case not in {"disabled", "menu-on"},
                "base_url": f"http://127.0.0.1:{server.server_port}/v1/",
                "model": "local-test-model",
                "candidates": 3,
                "context_chars": 256,
            }
            if case == "api-auth":
                settings["api_key"] = "local-test-key"
            if case == "api-timeout":
                settings["timeout_ms"] = 150
            if case == "invalid-config":
                settings["candidates"] = 99
            text = default_text
            for key, value in settings.items():
                # JSON 标量也是 YAML 标量，无需 Python YAML 依赖。
                text = re.sub(rf"^{key}:.*$", f"{key}: {json.dumps(value)}",
                              text, flags=re.MULTILINE)
            if case == "partial-config":
                text = json.dumps({"candidates": 3, "temperature": 0.7, "api_key": ""})
            elif case == "empty-config":
                text = ""
            config.write_text(text, encoding="utf-8")
        (rime / "default.yaml").write_text(
            'config_version: "1"\nschema_list:\n  - schema: rome_ai_test\n', encoding="utf-8")
        (rime / "rome_ai_test.schema.yaml").write_text('''schema:
  schema_id: rome_ai_test
  name: Rome AI isolation test
  version: "1"
switches:
  - name: ascii_mode
    reset: 0
engine:
  processors: [ascii_composer, speller, selector, express_editor]
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [table_translator]
ascii_composer:
  switch_key:
    Shift_L: commit_code
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz
translator:
  dictionary: rome_ai_test
  enable_sentence: false
  enable_user_dict: false
menu:
  page_size: 3
''', encoding="utf-8")
        (rime / "rome_ai_test.dict.yaml").write_text('''---
name: rome_ai_test
version: "1"
sort: by_weight
...
一起\tyiqi\t100
你好\tnihao\t100
你\tn\t100
''', encoding="utf-8")
        profile = root / "config/fcitx5/profile"
        profile.write_text('''[Groups/0]
Name=Default
Default Layout=us
DefaultIM=rime

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=rime
Layout=

[GroupOrder]
0=Default
''', encoding="utf-8")
        if case == "api-probe":
            result = subprocess.run([probe, str(config), "今天晚上一起"],
                env=env, timeout=15, capture_output=True, text=True)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode == 0:
                response = json.loads(result.stdout)
                assert response["candidates"] == ["吃", "看", "去"]
                assert response["elapsed_ms"] >= 200
        else:
            result = subprocess.run([binary, case], env=env, timeout=15)
        assert not errors, errors
        no_request = {"disabled", "no-extension", "missing-config", "invalid-config", "invalid-global",
                      "no-context", "timeout", "other-im"}
        if case in no_request:
            assert not requests, f"{case}: must not call API"
        elif result.returncode == 0:
            assert requests, f"{case}: no actual HTTP request"
            expected_prefix = ("今天一起" if case == "selection" else
                               "一起" if case == "partial-config" else "今天晚上一起")
            if case != "unicode":
                assert requests[0]["prompt"] == expected_prefix
        sys.exit(result.returncode)
finally:
    server.shutdown()
    server.server_close()
    thread.join()
