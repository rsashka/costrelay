#!/usr/bin/env python3
"""Minimal RouterAI-compatible mock for local demos/tests.

Serves:
  GET  /api/v1/models
  GET  /api/v1/models/<author>/<model>/endpoints
  POST /api/v1/chat/completions (JSON and SSE)
"""
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("MOCK_ROUTERAI_PORT", "19090"))
FAIL_PROVIDERS = {p.strip() for p in os.environ.get("MOCK_FAIL_PROVIDERS", "").split(",") if p.strip()}


def _flaky_spec() -> dict:
    # "fast:1,slow:2" — провайдер отвечает 500 первые N раз, затем успешно.
    out = {}
    for spec in os.environ.get("MOCK_FLAKY_PROVIDERS", "").split(","):
        spec = spec.strip()
        if not spec:
            continue
        name, _, count = spec.partition(":")
        try:
            out[name.strip()] = int(count or "1")
        except ValueError:
            out[name.strip()] = 1
    return out


FLAKY_REMAINING = _flaky_spec()
FLAKY_LOCK = threading.Lock()


def _empty_spec() -> dict:
    # "fast:2" — провайдер первые N раз отвечает 200 с пустым выводом.
    out = {}
    for spec in os.environ.get("MOCK_EMPTY_PROVIDERS", "").split(","):
        spec = spec.strip()
        if not spec:
            continue
        name, _, count = spec.partition(":")
        try:
            out[name.strip()] = int(count or "1")
        except ValueError:
            out[name.strip()] = 1
    return out


EMPTY_REMAINING = _empty_spec()
EMPTY_LOCK = threading.Lock()

ENDPOINTS = {
    "mock/model-a": {
        "data": {
            "id": "mock/model-a",
            "name": "Mock Model A",
            "architecture": {"input_modalities": ["text"], "output_modalities": ["text"]},
            "endpoints": [
                {
                    "name": "Fast provider",
                    "provider_name": "Fast",
                    "tag": "fast",
                    "country": "ru",
                    "countries": ["ru"],
                    "context_length": 128000,
                    "max_completion_tokens": 4096,
                    "supported_parameters": [
                        "tools", "tool_choice", "response_format", "temperature",
                        "top_p", "max_tokens", "stream", "seed", "stop"
                    ],
                    "status": 0,
                    "pricing": {"prompt": 0.0001, "completion": 0.0003},
                    "pricing_units": {"prompt": "token", "completion": "token"},
                },
                {
                    "name": "Slow provider",
                    "provider_name": "Slow",
                    "tag": "slow",
                    "country": "ru",
                    "countries": ["ru"],
                    "context_length": 128000,
                    "max_completion_tokens": 4096,
                    "supported_parameters": [
                        "tools", "tool_choice", "response_format", "temperature",
                        "top_p", "max_tokens", "stream", "seed", "stop"
                    ],
                    "status": 0,
                    "pricing": {"prompt": 0.0002, "completion": 0.0004},
                    "pricing_units": {"prompt": "token", "completion": "token"},
                },
            ],
        }
    }
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] " + fmt % args + "\n")

    def _json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/v1/models":
            return self._json(200, {"data": [{"id": "mock/model-a", "name": "Mock Model A"}]})
        prefix = "/api/v1/models/"
        suffix = "/endpoints"
        if path.startswith(prefix) and path.endswith(suffix):
            model = path[len(prefix):-len(suffix)]
            if model in ENDPOINTS:
                return self._json(200, ENDPOINTS[model])
            return self._json(404, {"error": {"message": "model not found"}})
        return self._json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path != "/api/v1/chat/completions":
            return self._json(404, {"error": {"message": "not found"}})
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw)
        except Exception:
            return self._json(400, {"error": {"message": "invalid json"}})
        provider = req.get("provider") or {}
        chosen = None
        if isinstance(provider.get("only"), list) and provider["only"]:
            chosen = provider["only"][0]
        elif isinstance(provider.get("order"), list) and provider["order"]:
            chosen = provider["order"][0]
        if chosen not in ("fast", "slow"):
            return self._json(404, {"error": {"message": "no provider", "provider": provider}})
        model = req.get("model", "mock/model-a")
        stream = bool(req.get("stream", False))
        if chosen in FAIL_PROVIDERS:
            return self._json(500, {"error": {"message": "injected provider failure", "provider": chosen}})
        with FLAKY_LOCK:
            remaining = FLAKY_REMAINING.get(chosen, 0)
            if remaining > 0:
                FLAKY_REMAINING[chosen] = remaining - 1
                injected = True
            else:
                injected = False
        if injected:
            return self._json(500, {"error": {"message": "injected flaky failure", "provider": chosen}})
        with EMPTY_LOCK:
            empty_left = EMPTY_REMAINING.get(chosen, 0)
            if empty_left > 0:
                EMPTY_REMAINING[chosen] = empty_left - 1
                empty = True
            else:
                empty = False
        if empty and not stream:
            return self._json(200, {
                "id": "chatcmpl-empty",
                "object": "chat.completion",
                "model": model,
                "choices": [],
                "provider_echo": chosen,
            })
        if not stream:
            return self._json(200, {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "model": model,
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": f"hello from {chosen}"},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 12, "completion_tokens": 5, "total_tokens": 17},
                "provider_echo": chosen,
            })
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        def chunk(data: str):
            b = data.encode()
            self.wfile.write(f"{len(b):X}\r\n".encode() + b + b"\r\n")
            self.wfile.flush()
        if empty:
            chunk(f'data: {{"id":"chatcmpl-empty","model":"{model}","choices":[{{"index":0,"delta":{{"role":"assistant"}},"finish_reason":null}}]}}\n\n')
            chunk(f'data: {{"id":"chatcmpl-empty","model":"{model}","choices":[{{"index":0,"delta":{{}},"finish_reason":"stop"}}]}}\n\n')
        else:
            chunk(f'data: {{"id":"chatcmpl-mock","model":"{model}","choices":[{{"index":0,"delta":{{"role":"assistant","content":"hello "}}}}]}}\n\n')
            chunk(f'data: {{"id":"chatcmpl-mock","model":"{model}","choices":[{{"index":0,"delta":{{"content":"from {chosen}"}}}}]}}\n\n')
            chunk('data: {"id":"chatcmpl-mock","choices":[],"usage":{"prompt_tokens":12,"completion_tokens":5,"total_tokens":17}}\n\n')
        chunk("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


def main():
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    server.daemon_threads = True
    print(f"mock RouterAI on http://127.0.0.1:{PORT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
