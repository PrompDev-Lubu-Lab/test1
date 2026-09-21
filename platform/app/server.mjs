import { createServer as createHTTPServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { resolve, relative, extname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const publicRoot = fileURLToPath(new URL('./public/',import.meta.url));
const MIME = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.svg':'image/svg+xml','.ttf':'font/ttf','.json':'application/json; charset=utf-8','.txt':'text/plain; charset=utf-8'};
const CSP = "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self'; font-src 'self'; connect-src 'self'; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'none'";
const readonlyPaths = /^\/(?:instances(?:\/[A-Za-z0-9_.-]+(?:\/(?:status|state|journal|metrics\.prom))?)?|runs(?:\/[A-Za-z0-9_.-]+(?:\/(?:equity|fills|orders|metrics|round_trips|report|drift|revalidation|validation))?)?|board\/(?:tasks|notes))$/;
function sendJSON(res,status,body) { res.writeHead(status,{'Content-Type':MIME['.json']}); res.end(JSON.stringify(body)); }
export function createAppServer({syntheticPreview = false,apiOrigin = 'http://127.0.0.1:8788',root = publicRoot} = {}) {
  const upstream = new URL(apiOrigin);
  if (upstream.protocol !== 'http:' || upstream.hostname !== '127.0.0.1' || upstream.username || upstream.password || upstream.pathname !== '/' || upstream.search || upstream.hash) throw new Error('The development API must use an explicit loopback HTTP origin.');
  const directory = resolve(root);
  return createHTTPServer(async (req,res) => {
    res.setHeader('Cache-Control','no-store');
    res.setHeader('Content-Security-Policy',CSP);
    res.setHeader('X-Content-Type-Options','nosniff');
    res.setHeader('Referrer-Policy','no-referrer');
    res.setHeader('X-Frame-Options','DENY');
    const address = res.socket?.localPort;
    if (![`127.0.0.1:${address}`,`localhost:${address}`].includes(req.headers.host)) return sendJSON(res,403,{error:'host_denied',detail:'Only the local development host is allowed.'});
    const origin = req.headers.origin;
    if (origin && ![`http://127.0.0.1:${address}`,`http://localhost:${address}`].includes(origin)) return sendJSON(res,403,{error:'origin_denied',detail:'Cross-origin requests are not allowed.'});
    if (req.headers['sec-fetch-site'] === 'cross-site') return sendJSON(res,403,{error:'origin_denied',detail:'Cross-site requests are not allowed.'});
    if (!['GET','HEAD'].includes(req.method)) { res.setHeader('Allow','GET, HEAD'); return sendJSON(res,405,{error:'read_only',detail:'This presentation server accepts no writes.'}); }
    try {
      const url = new URL(req.url,'http://127.0.0.1');
      if (url.pathname === '/runtime-config.json') return sendJSON(res,200,{syntheticPreview:syntheticPreview === true,access:syntheticPreview ? 'local-synthetic-preview' : 'closed'});
      if (url.pathname.startsWith('/api/')) {
        if (!syntheticPreview) return sendJSON(res,503,{error:'access_not_configured',detail:'Production access remains closed until the account Worker is configured.'});
        let path;
        try { path = decodeURIComponent(url.pathname.slice(4)); } catch { return sendJSON(res,400,{error:'invalid_path',detail:'The API path is invalid.'}); }
        if (!readonlyPaths.test(path) || path.split('/').some(part => part === '..' || part === '.')) return sendJSON(res,404,{error:'not_found',detail:'This read-only endpoint is not available.'});
        const target = new URL(path,upstream);
        for (const [key,value] of url.searchParams) {
          if (!['every','after','offset','limit','name'].includes(key) || value.length > 100 || url.searchParams.getAll(key).length !== 1) return sendJSON(res,400,{error:'invalid_query',detail:'The query is not supported.'});
          const maximum = {every:1000000,after:1000000000,offset:Number.MAX_SAFE_INTEGER,limit:10000}[key];
          if (maximum !== undefined && (!/^\d+$/.test(value) || !Number.isSafeInteger(Number(value)) || Number(value) > maximum || (!['after','offset'].includes(key) && Number(value) === 0))) return sendJSON(res,400,{error:'invalid_query',detail:'The pagination value is outside the supported range.'});
          target.searchParams.append(key,value);
        }
        let response;
        try { response = await fetch(target,{method:req.method,redirect:'error',signal:AbortSignal.timeout(10000),headers:{Accept:'application/json, text/plain'}}); }
        catch { return sendJSON(res,503,{error:'api_unavailable',detail:'The read-only Run API is unavailable. Start it on 127.0.0.1:8788 and refresh.'}); }
        const parts=[]; let length=0;
        if (response.body) for await (const part of response.body) {
          length += part.length;
          if (length > 12 * 1024 * 1024) throw new Error('API response exceeds the preview limit.');
          parts.push(part);
        }
        const headers = {'Content-Type':response.headers.get('Content-Type') || MIME['.json']};
        for (const name of ['X-Data-Source','X-Next-After','X-Next-Offset','X-Has-More','X-Result-Warning']) {
          const value = response.headers.get(name);
          if (value !== null) headers[name] = value;
        }
        res.writeHead(response.status,headers);
        res.end(req.method === 'HEAD' ? undefined : Buffer.concat(parts));
        return;
      }
      let decoded;
      try { decoded = decodeURIComponent(url.pathname); } catch { return sendJSON(res,400,{error:'invalid_path'}); }
      if (decoded.includes('\\') || decoded.includes('\0') || decoded.split('/').some(part => part.startsWith('.'))) return sendJSON(res,404,{error:'not_found'});
      const path = resolve(directory,decoded === '/' ? 'index.html' : `.${decoded}`);
      const inside = relative(directory,path);
      if (!inside || inside.startsWith('..') || !MIME[extname(path)]) return sendJSON(res,404,{error:'not_found'});
      const info = await stat(path).catch(() => null);
      if (!info?.isFile()) return sendJSON(res,404,{error:'not_found'});
      const content = await readFile(path);
      res.writeHead(200,{'Content-Type':MIME[extname(path)],'Content-Length':content.length});
      res.end(req.method === 'HEAD' ? undefined : content);
    } catch { if (!res.headersSent) sendJSON(res,503,{error:'unavailable',detail:'This output could not be served.'}); else res.end(); }
  });
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const port = Number(process.env.PORT || '8787');
  if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('PORT must be between 1024 and 65535.');
  const syntheticPreview = process.argv.includes('--preview');
  const server = createAppServer({syntheticPreview});
  server.listen(port,'127.0.0.1',() => console.log(`Presentation: http://127.0.0.1:${port} (${syntheticPreview ? 'explicit synthetic preview' : 'production access closed'})`));
  for (const signal of ['SIGINT','SIGTERM']) process.on(signal,() => server.close(() => process.exit(0)));
}
