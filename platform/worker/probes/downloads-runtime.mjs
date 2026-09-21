// Local synthetic workerd/R2 probe only. Never configure this as a deployment entry point.
// These explicit harness headers are not Access, account sessions, or production authorization.
import { readDownloadCatalog, serveRelease } from '../downloads.mjs';
import { SecurityError } from '../security.mjs';

export default {
  async fetch(request, env) {
    const authorization = request.headers.get('X-Probe-Authorization');
    const options = authorization === 'omit' ? undefined : {
      authorize: async () => {
        if (authorization !== 'synthetic-allow') throw new SecurityError('login_required', 401);
      },
    };
    const runtimeEnv = request.headers.get('X-Probe-Disable') === 'true'
      ? { ...env, DOWNLOADS_READY: 'disabled' } : env;
    try {
      if (new URL(request.url).pathname === '/api/downloads') {
        return Response.json(await readDownloadCatalog(runtimeEnv, options), {
          headers: { 'Cache-Control': 'private, no-store' },
        });
      }
      return await serveRelease(request, runtimeEnv, options);
    } catch (error) {
      return Response.json({ error: error.code || 'probe_failed' }, { status: error.status || 503 });
    }
  },
};
