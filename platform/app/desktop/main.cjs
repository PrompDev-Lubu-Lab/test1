const {app,BrowserWindow,shell,session} = require('electron');
const {join} = require('node:path');
const {pathToFileURL} = require('node:url');
let server;
// No trading IPC, local case service, shell commands, credentials or update feeds.
const externalOrigins = new Set(['https://clawdie.ai']);
async function openAllowedExternal(value) {
  try { const url = new URL(value); if (externalOrigins.has(url.origin) && url.protocol === 'https:' && !url.username && !url.password) await shell.openExternal(url.href); } catch { /* Deny unsupported links. */ }
}
app.setName('clawdie-platform');
app.whenReady().then(async () => {
  const {createAppServer} = await import(pathToFileURL(join(__dirname,'../server.mjs')).href);
  const syntheticPreview = !app.isPackaged && process.argv.includes('--preview');
  server = createAppServer({syntheticPreview});
  await new Promise((resolve,reject) => { server.once('error',reject); server.listen(0,'127.0.0.1',resolve); });
  const origin = `http://127.0.0.1:${server.address().port}`;
  const isolated = session.fromPartition('clawdie-platform-session');
  isolated.setPermissionRequestHandler((_contents,_permission,callback) => callback(false));
  isolated.setPermissionCheckHandler(() => false);
  isolated.on('will-download',event => event.preventDefault());
  const window = new BrowserWindow({width:1380,height:900,minWidth:820,minHeight:650,backgroundColor:'#183a3d',show:false,webPreferences:{preload:join(__dirname,'preload.cjs'),session:isolated,contextIsolation:true,nodeIntegration:false,sandbox:true,webSecurity:true,allowRunningInsecureContent:false}});
  window.webContents.setWindowOpenHandler(({url}) => { void openAllowedExternal(url); return {action:'deny'}; });
  window.webContents.on('will-navigate',(event,url) => { if (new URL(url).origin !== origin) { event.preventDefault(); void openAllowedExternal(url); } });
  window.webContents.on('will-attach-webview',event => event.preventDefault());
  window.once('ready-to-show',() => window.show());
  await window.loadURL(origin);
});
app.on('window-all-closed',() => app.quit());
app.on('before-quit',() => server?.close());
