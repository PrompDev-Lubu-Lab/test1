import { pbkdf2Sync } from 'node:crypto';
import { pbkdf2 } from '@noble/hashes/pbkdf2.js';
import { sha256 } from '@noble/hashes/sha2.js';
// Run only through Wrangler's authenticated remote preview. No passwords or users.
export default {
  async fetch(request) {
    if (new URL(request.url).pathname !== '/probe' || request.method !== 'POST') {
      return new Response('Not found', { status: 404 });
    }
    const iterations = 600000;
    const requested = new URL(request.url).searchParams.get('implementation');
    const implementation = requested === 'node' ? 'node:crypto' : requested === 'noble' ? '@noble/hashes@2.4.0' : 'WebCrypto';
    try {
      if (requested === 'noble') {
        const bits = pbkdf2(sha256, new TextEncoder().encode('public-synthetic-benchmark-input'), new Uint8Array(16).fill(19), { c: iterations, dkLen: 32 });
        return Response.json({ supported: true, implementation, iterations, byte_length: bits.byteLength, output_hex: Array.from(bits, byte => byte.toString(16).padStart(2, '0')).join(''), note: 'Synthetic known input only; external elapsed time is not CPU billing.' }, { headers: { 'Cache-Control': 'no-store' } });
      }
      if (implementation === 'node:crypto') {
        const bits = pbkdf2Sync('public-synthetic-benchmark-input', new Uint8Array(16).fill(19), iterations, 32, 'sha256');
        return Response.json({ supported: true, implementation, iterations, byte_length: bits.byteLength, output_hex: bits.toString('hex'), note: 'Synthetic known input only; external elapsed time is not CPU billing.' }, { headers: { 'Cache-Control': 'no-store' } });
      }
      const key = await crypto.subtle.importKey('raw', new TextEncoder().encode('public-synthetic-benchmark-input'), 'PBKDF2', false, ['deriveBits']);
      const start = performance.now();
      const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: new Uint8Array(16).fill(19), iterations }, key, 256);
      return Response.json({ supported: true, implementation, iterations, byte_length: bits.byteLength, observed_ms: performance.now() - start, note: 'Worker clock observations are not a CPU billing measurement.' }, { headers: { 'Cache-Control': 'no-store' } });
    } catch (error) {
      return Response.json({ supported: false, implementation, iterations, error: error.name, detail: error.message }, { headers: { 'Cache-Control': 'no-store' } });
    }
  }
};
