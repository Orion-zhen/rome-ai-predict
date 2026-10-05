#!/usr/bin/env python3
"""真实 librime 组件 + 模拟 AX 控件 + 本地 HTTP，不连接 macOS 或桌面输入法。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary, work, scenario = sys.argv[1:]
requests = []
errors = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(body)
        if self.path != "/v1/completions" or body["model"] != "native-test-model":
            errors.append("wrong endpoint/model")
        if body["max_tokens"] != 1 or body["logprobs"] != 3 or body["stream"] is not False:
            errors.append("wrong generation parameters")
        if "尾巴" in body["prompt"] or "旧词" in body["prompt"] or "yiqi" in body["prompt"]:
            errors.append("suffix, old selection or marked text leaked into prompt")
        if len(body["prompt"]) > 256:
            errors.append("context limit ignored")
        expected = "今天晚上一起Ａ" if scenario == "formatted" else "今天晚上一起"
        if scenario == "unicode":
            if len(body["prompt"]) != 256 or not body["prompt"].endswith(expected):
                errors.append("Unicode slicing failed")
        elif body["prompt"] not in {expected, expected + "吃"}:
            errors.append("prompt does not match application text")
        time.sleep(0.1)
        first = "吃A" if scenario == "formatted" else "吃"
        top = [{"token": text, "bytes": list(text.encode()), "logprob": score}
               for text, score in [("看", -1.1), ("去", -2.1), (first, -0.1)]]
        response = {"choices": [{"logprobs": {"content": [{"top_logprobs": top}]}}]}
        status = 200
        if scenario == "api-error":
            status, response = 503, {"error": "private-response-body"}
        data = json.dumps(response, ensure_ascii=False).encode()
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix=f"rime-{scenario}-", dir=work) as temporary:
        root = Path(temporary)
        config = {
            "enabled": True, "base_url": f"http://127.0.0.1:{server.server_port}/v1",
            "model": "native-test-model", "candidates": 3, "context_chars": 256,
        }
        if scenario == "invalid-config":
            config["candidates"] = 99
        (root / "rome-ai-predict.yaml").write_text(json.dumps(config), encoding="utf-8")
        (root / "default.yaml").write_text('''config_version: "1"
schema_list:
  - schema: rome_ai_test
switcher:
  hotkeys: [F4]
''', encoding="utf-8")
        (root / "rome_ai_test.schema.yaml").write_text(f'''schema:
  schema_id: rome_ai_test
  name: Rome AI plugin test
  version: "1"
switches:
  - name: ascii_mode
    reset: 0
  - name: full_shape
    reset: {1 if scenario == "formatted" else 0}
  - name: rome_ai_predict
    reset: 0
    states: [AI关, AI开]
engine:
  processors: [rome_ai_predictor, ascii_composer, speller, selector, express_editor]
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [table_translator]
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz
translator:
  dictionary: rome_ai_test
  enable_sentence: false
  enable_user_dict: false
menu:
  page_size: 3
''', encoding="utf-8")
        word = "一起A" if scenario == "formatted" else "一起"
        (root / "rome_ai_test.dict.yaml").write_text(f'''---
name: rome_ai_test
version: "1"
sort: by_weight
...
{word}\tyiqi\t100
你\tn\t100
''', encoding="utf-8")
        env = {**os.environ, "HOME": temporary, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(name, None)
        result = subprocess.run([binary, temporary, scenario], capture_output=True, text=True,
                                timeout=12, env=env)
        print(result.stdout, end="")
        if result.returncode:
            print(result.stderr, end="", file=sys.stderr)
        assert result.returncode == 0, scenario
        assert not errors, errors
        assert "private-response-body" not in result.stdout + result.stderr
        no_request = {"no-context", "disabled", "ascii", "invalid-config", "no-ack", "unexpected-edit"}
        assert len(requests) == (0 if scenario in no_request else 2 if scenario == "continued" else 1), requests
finally:
    server.shutdown()
    server.server_close()
    thread.join()
