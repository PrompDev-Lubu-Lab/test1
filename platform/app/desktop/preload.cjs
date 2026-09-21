const {contextBridge} = require('electron');
// Presentation metadata only. Renderer requests cannot mutate the bot or OS.
contextBridge.exposeInMainWorld('platformDesktop',Object.freeze({isDesktop:true,updateSupport:'not-configured'}));
