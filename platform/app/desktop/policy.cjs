'use strict';
const {createHash} = require('node:crypto');
const UPDATE_PATH = '/api/updates/windows/x64/';
const CACHE_NAME = 'clawdie-platform-app-updater';
const COOKIE_NAMES = ['CF_Authorization', '__Host-platform-session'];
const VERSION = /^(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})$/;
function exactOrigin(value) {
  const url = new URL(value);
  if (url.protocol !== 'https:' || url.origin !== value || url.username || url.password || url.port) throw new Error('Use an exact HTTPS origin without a custom port.');
  return url.origin;
}
function validateSettings(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input) || input.schema !== 1 || !['review','production'].includes(input.channel)) throw new Error('Desktop settings are missing or unsupported.');
  const appOrigin = exactOrigin(input.appOrigin), accessOrigin = exactOrigin(input.accessOrigin);
  if (appOrigin === accessOrigin || !new URL(accessOrigin).hostname.endsWith('.cloudflareaccess.com')) throw new Error('Use the dedicated Cloudflare Access team origin.');
  if (!Array.isArray(input.externalOrigins) || input.externalOrigins.length > 8) throw new Error('Supply an explicit external-link allowlist.');
  const externalOrigins = input.externalOrigins.map(exactOrigin);
  if (typeof input.updatesEnabled !== 'boolean' || !Array.isArray(input.publisherNames) || !Array.isArray(input.certificateThumbprints)) throw new Error('Supply an explicit update policy.');
  const publisherNames = input.publisherNames;
  const certificateThumbprints = input.certificateThumbprints;
  if (publisherNames.length > 4 || publisherNames.some(v => typeof v !== 'string' || !v.trim() || v.length > 200 || /[\x00-\x1f\x7f]/.test(v)) || certificateThumbprints.length > 4 || certificateThumbprints.some(v => typeof v !== 'string' || !/^[A-F0-9]{40}$/.test(v))) throw new Error('Invalid signing allowlist.');
  if (input.updatesEnabled && (input.channel !== 'production' || !publisherNames.length || !certificateThumbprints.length)) throw new Error('Production updates require signing identity and certificate pins.');
  if (input.channel === 'production' && (new URL(appOrigin).hostname.endsWith('.test') || new URL(appOrigin).hostname === 'localhost')) throw new Error('Production needs the approved deployed app origin.');
  return Object.freeze({schema:1,channel:input.channel,appOrigin,accessOrigin,externalOrigins:Object.freeze(externalOrigins),updatesEnabled:input.updatesEnabled,publisherNames:Object.freeze([...publisherNames]),certificateThumbprints:Object.freeze([...certificateThumbprints]),feedUrl:appOrigin+UPDATE_PATH,cacheName:CACHE_NAME});
}
function allowedNavigation(value, settings) {
  try {
    const url = new URL(value);
    if (url.protocol !== 'https:' || url.username || url.password || url.port) return false;
    if (url.origin === settings.appOrigin) return true;
    if (url.origin === settings.accessOrigin) return /^\/cdn-cgi\/access(?:\/|$)/.test(url.pathname);
    // Human GitHub OAuth screens; no repository pages or arbitrary domains.
    if (url.origin === 'https://github.com') return /^\/(?:login(?:\/|$)|session(?:\/|$)|sessions(?:\/|$))/.test(url.pathname);
  } catch { /* Deny malformed and non-HTTPS navigation. */ }
  return false;
}
function allowedExternal(value, settings) {
  try { const url = new URL(value); return url.protocol === 'https:' && !url.username && !url.password && settings.externalOrigins.includes(url.origin); } catch { return false; }
}
function allowedUpdateUrl(value, settings) {
  try {
    const url = new URL(value);
    if (url.origin !== settings.appOrigin || url.username || url.password || url.hash || /[\\%]/.test(url.pathname) || !url.pathname.startsWith(UPDATE_PATH)) return false;
    const name = url.pathname.slice(UPDATE_PATH.length);
    if (name !== 'latest.yml' && !/^clawdie-platform-(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})-win-x64\.exe$/.test(name)) return false;
    const query = [...url.searchParams.entries()];
    return !query.length || (name === 'latest.yml' && query.length === 1 && query[0][0] === 'noCache' && /^[A-Za-z0-9_-]{1,100}$/.test(query[0][1]));
  } catch { return false; }
}
function sessionCookies(cookies, appOrigin, now = Date.now()) {
  const hostname = new URL(appOrigin).hostname;
  const selected = COOKIE_NAMES.map(name => {
    const matches = cookies.filter(c => c.name === name);
    if (matches.length !== 1) throw new Error('Sign in before checking for updates.');
    const c = matches[0];
    if (!c.secure || !c.httpOnly || c.path !== '/' || (c.expirationDate !== undefined && c.expirationDate <= now / 1000) || typeof c.value !== 'string' || !c.value || c.value.length > 16384 || !/^[A-Za-z0-9._~-]+$/.test(c.value)) throw new Error('The current session is unavailable.');
    if (name.startsWith('__Host-') && (c.domain !== hostname || c.hostOnly !== true)) throw new Error('The app session is not bound to this host.');
    return `${name}=${c.value}`;
  });
  const header = selected.join('; ');
  const expiresAt=Math.min(...cookies.filter(c=>COOKIE_NAMES.includes(c.name)).map(c=>c.expirationDate===undefined?Infinity:c.expirationDate*1000));
  return {header,fingerprint:createHash('sha256').update(header).digest('hex'),expiresAt};
}
function validateUpdateInfo(info, settings, currentVersion) {
  if (!info || !VERSION.test(info.version) || !VERSION.test(currentVersion)) throw new Error('Unsupported release version.');
  const next = info.version.split('.').map(Number), current = currentVersion.split('.').map(Number);
  let comparison = 0;
  for (let i=0;i<3;i++) if (next[i] !== current[i]) { comparison = next[i] > current[i] ? 1 : -1; break; }
  if (comparison <= 0) throw new Error('Only a newer stable release can be installed.');
  if (!Array.isArray(info.files) || info.files.length !== 1 || info.packages != null) throw new Error('Use one complete Windows installer.');
  const file = info.files[0], expected = `clawdie-platform-${info.version}-win-x64.exe`;
  if (file.url !== expected || file.isAdminRightsRequired === true || info.isAdminRightsRequired === true || !allowedUpdateUrl(settings.feedUrl + file.url,settings) || typeof file.sha512 !== 'string' || !/^[A-Za-z0-9+/]{86}==$/.test(file.sha512) || !Number.isSafeInteger(file.size) || file.size < 1 || file.size > 1073741824) throw new Error('Release artifact metadata is invalid.');
  if (info.path !== undefined && info.path !== expected) throw new Error('Conflicting release paths.');
  if (info.sha512 !== undefined && info.sha512 !== file.sha512) throw new Error('Conflicting release checksums.');
  return Object.freeze({version:info.version,file:expected,size:file.size,sha512:file.sha512});
}
module.exports = {UPDATE_PATH,CACHE_NAME,COOKIE_NAMES,validateSettings,allowedNavigation,allowedExternal,allowedUpdateUrl,sessionCookies,validateUpdateInfo};
