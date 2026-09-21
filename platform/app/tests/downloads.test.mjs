import test from 'node:test';
import assert from 'node:assert/strict';
import {validateDownload} from '../public/downloads.js';
test('download presentation accepts only a version-matched local artifact and bounded metadata',()=>{
  const value={version:'0.1.1',file:'clawdie-platform-0.1.1-win-x64.exe',url:'/api/updates/windows/x64/clawdie-platform-0.1.1-win-x64.exe',size:42,sha512:Buffer.alloc(64).toString('base64'),released_at:'2026-09-21T00:00:00Z',notes:'Release details'};
  assert.equal(validateDownload(value).url,value.url);
  for(const change of [{url:'https://other.example.test/setup.exe'},{url:'//other.example.test/setup.exe'},{version:'0.1.2'},{file:'../setup.exe'},{size:Infinity},{size:1073741825},{sha512:'wrong'},{released_at:'missing'}])assert.throws(()=>validateDownload({...value,...change}));
});
