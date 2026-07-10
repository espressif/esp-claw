import http from "node:http";
import https from "node:https";

const port = Number(process.env.PORT || 8092);

const send = (res, status, body, headers = {}) => {
  res.writeHead(status, {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,HEAD,OPTIONS",
    "access-control-allow-headers": "range,content-type",
    ...headers,
  });
  if (body != null) {
    res.end(body);
  } else {
    res.end();
  }
};

const proxy = (req, res, target) => {
  const client = target.protocol === "https:" ? https : http;
  const headers = {
    "user-agent": "ESP-Claw-Web-Sim/1.0",
    "accept": req.headers.accept || "*/*",
    "icy-metadata": "0",
  };
  if (req.headers.range) {
    headers.range = req.headers.range;
  }

  const upstream = client.request(target, { method: req.method, headers }, (upstreamRes) => {
    res.writeHead(upstreamRes.statusCode || 502, {
      "access-control-allow-origin": "*",
      "access-control-expose-headers": "content-length,content-range,accept-ranges,content-type",
      "content-type": upstreamRes.headers["content-type"] || "audio/mpeg",
      "accept-ranges": upstreamRes.headers["accept-ranges"] || "bytes",
      ...(upstreamRes.headers["content-length"] ? { "content-length": upstreamRes.headers["content-length"] } : {}),
      ...(upstreamRes.headers["content-range"] ? { "content-range": upstreamRes.headers["content-range"] } : {}),
      ...(upstreamRes.headers["icy-name"] ? { "icy-name": upstreamRes.headers["icy-name"] } : {}),
    });
    if (req.method === "HEAD") {
      res.end();
    } else {
      upstreamRes.pipe(res);
    }
  });

  upstream.on("error", (err) => {
    if (!res.headersSent) {
      send(res, 502, `proxy upstream error: ${err.message}\n`);
    } else {
      res.destroy(err);
    }
  });
  req.on("close", () => upstream.destroy());
  upstream.end();
};

const server = http.createServer((req, res) => {
  if (req.method === "OPTIONS") {
    send(res, 204);
    return;
  }
  const requestUrl = new URL(req.url || "/", `http://${req.headers.host || "127.0.0.1"}`);
  if (requestUrl.pathname !== "/stream") {
    send(res, 404, "not found\n");
    return;
  }
  const rawTarget = requestUrl.searchParams.get("url");
  if (!rawTarget) {
    send(res, 400, "missing url\n");
    return;
  }
  let target;
  try {
    target = new URL(rawTarget);
  } catch {
    send(res, 400, "invalid url\n");
    return;
  }
  if (target.protocol !== "http:" && target.protocol !== "https:") {
    send(res, 400, "unsupported protocol\n");
    return;
  }
  proxy(req, res, target);
});

server.listen(port, "127.0.0.1", () => {
  console.log(`[stream_proxy] listening on http://127.0.0.1:${port}`);
});
