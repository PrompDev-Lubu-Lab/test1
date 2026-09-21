"use strict";
const {app,BrowserWindow,shell,session,Menu,dialog,safeStorage} = require('electron');
const {join} = require('node:path');
const {readFile,lstat} = require('node:fs/promises');
const {pathToFileURL} = require('node:url');
const {validateSettings,allowedNavigation,allowedExternal} = require('./policy.cjs');
const {createDesktopUpdater} = require('./updater.cjs');
let server,desktopUpdater;
const reviewIdentity=!app.isPackaged || require('./settings.generated.json').channel==='review';
app.setName(reviewIdentity?'clawdie-platform-development':'clawdie-platform');
app.setPath('userData',join(app.getPath('appData'),app.getName()));
if(!app.requestSingleInstanceLock())app.quit();
else app.whenReady().then(async()=>{
  const {APP_NAME}=await import(pathToFileURL(join(__dirname,'../public/config.js')).href);
  const preview=!app.isPackaged && process.argv.includes('--preview');
  let settings,origin;
  if(preview) {
    const {createAppServer}=await import(pathToFileURL(join(__dirname,'../server.mjs')).href);
    server=createAppServer({syntheticPreview:true});
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    origin=`http://127.0.0.1:${server.address().port}`;
  } else {
    const configPath=app.isPackaged?join(__dirname,'settings.generated.json'):join(__dirname,'settings.local.json');
    const info=await lstat(configPath);
    if(!info.isFile() || info.isSymbolicLink() || info.size>16384)throw new Error('Invalid settings file.');
    settings=validateSettings(JSON.parse(await readFile(configPath,'utf8')));origin=settings.appOrigin;
    if(app.isPackaged && (process.platform!=='win32' || !safeStorage.isEncryptionAvailable()))throw new Error('This release requires Windows protected credential storage.');
  }
  const partition=preview?'platform-synthetic-preview':app.isPackaged?'persist:platform-human-session':'platform-human-development';
  const isolated=session.fromPartition(partition,{cache:false});
  isolated.setPermissionRequestHandler((_contents,_permission,callback)=>callback(false));
  isolated.setPermissionCheckHandler(()=>false);
  isolated.on('will-download',event=>event.preventDefault());
  const window=new BrowserWindow({title:APP_NAME+(preview || settings?.channel==='review'?' — Development':''),width:1380,height:900,minWidth:820,minHeight:650,backgroundColor:'#183a3d',show:false,webPreferences:{preload:join(__dirname,'preload.cjs'),session:isolated,contextIsolation:true,nodeIntegration:false,sandbox:true,webSecurity:true,allowRunningInsecureContent:false,spellcheck:false,devTools:!app.isPackaged}});
  app.on('second-instance',()=>{if(window.isMinimized())window.restore();window.focus();});
  async function external(value) {if(settings && allowedExternal(value,settings))await shell.openExternal(value);}
  const permitted=value=>{if(!preview)return allowedNavigation(value,settings);try{return new URL(value).origin===origin;}catch{return false;}};
  window.webContents.setWindowOpenHandler(({url})=>{void external(url);return {action:'deny'};});
  window.webContents.on('will-navigate',(event,url)=>{if(!permitted(url))event.preventDefault();});
  window.webContents.on('will-redirect',(event,url,_inPlace,isMainFrame)=>{if(isMainFrame && !permitted(url))event.preventDefault();});
  window.webContents.on('will-attach-webview',event=>event.preventDefault());
  window.webContents.on('page-title-updated',event=>event.preventDefault());
  window.once('ready-to-show',()=>window.show());
  let offline=false;
  async function connect() {offline=false;try{await window.loadURL(origin);}catch{if(!offline && !window.isDestroyed()){offline=true;await window.loadFile(join(__dirname,'offline.html'));window.show();}}}
  function menu(busy=false) {
    Menu.setApplicationMenu(Menu.buildFromTemplate([{label:APP_NAME,submenu:[{label:'Reconnect',click:()=>void connect()},{label:'Downloads',click:()=>void window.loadURL(origin+'/#downloads').catch(()=>connect())},{type:'separator'},{label:busy?'Update in progress…':'Check for updates',enabled:Boolean(desktopUpdater?.enabled && !busy),click:()=>void desktopUpdater.check()},{type:'separator'},{role:'quit'}]},{role:'editMenu'},{label:'View',submenu:[{role:'zoomIn'},{role:'zoomOut'},{role:'resetZoom'},{role:'togglefullscreen'}]}]));
  }
  if(!preview) {
    try{desktopUpdater=await createDesktopUpdater({app,appSession:isolated,settings,dialog,window,changed:menu});}
    catch{await dialog.showMessageBox(window,{type:'warning',message:'Desktop updates are disabled.',detail:'The packaged update configuration could not be verified. The web workspace can still be opened.'});}
  }
  menu();await connect();
}).catch(()=>{dialog.showErrorBox('Desktop configuration required','This desktop build cannot open the workspace until its approved HTTPS origin and access settings are configured. No account credentials were collected.');app.quit();});
app.on('window-all-closed',()=>app.quit());
app.on('before-quit',()=>{desktopUpdater?.dispose();server?.close();});
