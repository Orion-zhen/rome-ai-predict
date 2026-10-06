"""实际 Session + 内存控件 + 本地 Completions，不读取桌面或注入按键。"""
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


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(body)
        if self.path != '/v1/completions' or body.get('model') != 'weasel-session-test':
            errors.append('wrong protocol')
        if body.get('max_tokens') != 1 or body.get('n') != 1 or body.get('logprobs') != 2:
            errors.append('single-token Top-K contract changed')
        started.set()
        if scenario == 'late-model':
            release.wait(5)
        top = [{'token': text, 'bytes': list(text.encode()), 'logprob': score}
               for text, score in [('看', -2.0), ('吃', -1.0)]]
        data = json.dumps({'choices': [{'logprobs': {'content': [{'top_logprobs': top}]}}]}).encode()
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
process = None
try:
    process = subprocess.Popen([binary, scenario, f'http://127.0.0.1:{server.server_port}/v1'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8',
        env={**os.environ, 'NO_PROXY': '127.0.0.1', 'no_proxy': '127.0.0.1'})
    if scenario == 'late-model':
        assert process.stdout.readline().strip() == 'request-pending'
        assert started.wait(3), 'request not observed by local server'
        process.stdin.write('cancel\n'); process.stdin.flush()
        assert process.stdout.readline().strip() == 'cancelled'
        release.set()
        process.stdin.write('released\n'); process.stdin.flush()
    output, error = process.communicate(timeout=10)
    assert process.returncode == 0, output + error
    none = {'cold-missing', 'late-baseline', 'focus-lost', 'preview-block', 'read-race',
            'changed-target', 'changed-cursor', 'changed-suffix'}
    expected = [] if scenario in none else ['前文😀去']
    if scenario == 'menu-selection':
        expected.append('前文😀去吃')
    if scenario == 'cold-recovery':
        expected = ['前文😀去吃']
    assert [body['prompt'] for body in requests] == expected, 'unexpected or stale model request'
    assert not errors, errors
    print(output, end='')
finally:
    release.set()
    if process and process.poll() is None:
        process.kill()
        process.communicate()
    server.shutdown()
    server.server_close()
    thread.join()
