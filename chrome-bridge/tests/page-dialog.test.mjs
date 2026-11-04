import assert from 'node:assert/strict';
import test from 'node:test';
import {spawnSync} from 'node:child_process';
import {existsSync, mkdtempSync, writeFileSync, rmSync, lstatSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {basename, dirname, join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {pageTask} from '../extension/page.mjs';

// A separate, short-lived profile exercises real layout and native dialog focus.
// No extension is installed, no signed-in profile is opened and no port listens.
test('real Chrome observations prioritize visible modal dialogs on crowded pages', {timeout:45000}, async t => {
  const chrome=[process.env.DEVBOX_TEST_CHROME,
    process.env.ProgramFiles&&join(process.env.ProgramFiles,'Google/Chrome/Application/chrome.exe'),
    '/usr/bin/google-chrome','/usr/bin/chromium'].find(p=>p&&existsSync(p));
  assert(chrome,'Set DEVBOX_TEST_CHROME to a Chrome executable for this browser fixture');
  const parent=resolve(tmpdir()),directory=mkdtempSync(join(parent,'devbox-dialog-fixture-'));
  const program=String.raw`
    const background=document.createElement('section');
    const paragraph=document.createElement('p');paragraph.textContent='Earlier conversation '.repeat(1500);background.append(paragraph);
    for(let i=0;i<220;i++){const b=document.createElement('button');b.textContent='Background '+i;b.style.cssText='position:fixed;left:0;top:0';background.append(b);}
    document.body.append(background);
    const snapshots=[];
    const inspect=()=>{const r=pageTask({action:'inspect'});return {text:r.text,text_scope:r.text_scope,elements:r.elements,active_dialog:r.active_dialog,dialogs:r.dialogs,dialogs_truncated:r.dialogs_truncated};};
    const native=document.createElement('dialog');native.setAttribute('aria-labelledby','details-title');
    native.innerHTML='<h2 id="details-title">Details</h2><p>Only this status call needs approval.</p><button id="close">Close details</button>';
    document.body.append(native);native.showModal();let r=inspect();
    const close=r.elements.find(e=>e.label==='Close details');
    if(close){const point=pageTask({action:'point',target_id:close.id});r.close_hit=document.elementFromPoint(point.x,point.y)?.id;}
    snapshots.push(r);native.style.cssText='height:120px;overflow:auto';const long=document.createElement('div');long.style.height='1000px';long.textContent='Long modal contents';native.append(long);snapshots.push(inspect());
    native.close();native.remove();snapshots.push(inspect());
    const hidden=document.createElement('div');hidden.setAttribute('role','dialog');hidden.setAttribute('aria-modal','true');hidden.style.display='none';hidden.textContent='Hidden permission';document.body.append(hidden);
    const host=document.createElement('div');document.body.append(host);const shadow=host.attachShadow({mode:'open'});
    shadow.innerHTML='<div role="alertdialog" aria-modal="true" aria-labelledby="warning-title" style="position:fixed;inset:100px;background:white;z-index:1000"><h2 id="warning-title">Confirm action</h2><p>Review the requested operation.</p><button>Cancel operation</button></div>';
    shadow.querySelector('button').focus();snapshots.push(inspect());host.remove();hidden.remove();
    const outer=document.createElement('dialog');outer.setAttribute('aria-label','Outer dialog');outer.innerHTML='<button>Outer action</button>';document.body.append(outer);outer.showModal();
    const inner=document.createElement('dialog');inner.setAttribute('aria-label','Inner dialog');inner.innerHTML='<button>Inner action</button>';document.body.append(inner);inner.showModal();snapshots.push(inspect());inner.close();outer.close();inner.remove();outer.remove();
    const nonmodal=document.createElement('div');nonmodal.setAttribute('role','dialog');nonmodal.setAttribute('aria-label','Nonmodal panel');nonmodal.style.cssText='position:fixed;top:10px;left:10px';nonmodal.innerHTML='<button>Panel action</button>';document.body.append(nonmodal);snapshots.push(inspect());nonmodal.remove();
    const reported=[];for(let i=0;i<8;i++){const d=document.createElement('div');d.setAttribute('role','dialog');d.setAttribute('aria-label','Panel '+i);d.style.cssText='position:fixed;left:0;top:0;width:10px;height:10px';document.body.append(d);reported.push(d);}
    const overflow=document.createElement('dialog');overflow.setAttribute('aria-label','Overflow modal');overflow.innerHTML='<button>Close overflow</button>';document.body.append(overflow);overflow.showModal();snapshots.push(inspect());overflow.close();overflow.remove();for(const d of reported)d.remove();
    const lower=document.createElement('div');lower.setAttribute('role','dialog');lower.setAttribute('aria-modal','true');lower.setAttribute('aria-label','Covered lower modal');lower.style.cssText='position:fixed;inset:150px;background:white;z-index:1000';lower.innerHTML='<button>Lower action</button>';document.body.append(lower);
    const upper=lower.cloneNode(true);upper.setAttribute('aria-label','Covering modal');upper.style.zIndex='1001';upper.querySelector('button').textContent='Upper action';document.body.append(upper);lower.querySelector('button').focus();snapshots.push(inspect());upper.remove();lower.remove();
    background.replaceChildren();const many=document.createDocumentFragment();for(let i=0;i<20010;i++)many.append(document.createElement('div'));background.append(many);snapshots.push(inspect());
    return snapshots;
  `;
  const run=`const pageTask=(${pageTask.toString()});let result;try{result={snapshots:(()=>{${program}})()};}catch(e){result={error:String(e.stack||e)};}const output=document.createElement('pre');output.id='result';output.textContent=btoa(unescape(encodeURIComponent(JSON.stringify(result))));document.body.replaceChildren(output);`;
  const html=join(directory,'fixture.html');
  writeFileSync(html,'<!doctype html><meta charset="utf-8"><title>Owned Devbox dialog fixture</title><body><script>'+run.replaceAll('</script','<\\/script')+'</script>');
  try {
    const result=spawnSync(chrome,['--headless=new','--no-first-run','--no-default-browser-check','--disable-extensions','--disable-sync','--disable-background-networking','--disable-component-update','--window-size=1000,800',`--user-data-dir=${join(directory,'profile')}`,'--dump-dom',pathToFileURL(html).href],{encoding:'utf8',timeout:35000,maxBuffer:2*1024*1024,windowsHide:true});
    assert.ifError(result.error);assert.equal(result.status,0,result.stderr?.slice(-2000));
    const encoded=result.stdout.match(/<pre id="result">([A-Za-z0-9+/=]+)<\/pre>/)?.[1];
    assert(encoded,'Browser did not return the fixture result');
    const output=JSON.parse(Buffer.from(encoded,'base64').toString('utf8'));assert.equal(output.error,undefined);
    const [native,scrollRoot,plain,shadow,nested,nonmodal,overflow,covered,bounded]=output.snapshots;
    assert(native.elements.some(e=>e.label==='Close details'),'the existing observation must include the visible modal close button, even after 220 background controls');
    assert.equal(native.active_dialog?.label,'Details','a modal appended after 220 controls must be discovered');
    assert.match(native.text,/^Details\nOnly this status call needs approval/);
    assert.equal(native.text_scope,'active_dialog_then_rendered_document_prefix');
    assert.equal(native.close_hit,'close');
    assert(native.elements.some(e=>e.label==='Close details'));
    assert(!native.elements.some(e=>e.label.startsWith('Background ')),'inert background controls must not consume the active modal budget');
    assert(scrollRoot.elements.some(e=>e.tag==='dialog'&&e.scroll?.height>e.scroll.client_height),'the active modal itself remains a discoverable scrolling target');
    assert.equal(plain.active_dialog,null);assert.equal(plain.text_scope,'rendered_document_prefix');
    assert(plain.elements.some(e=>e.label.startsWith('Background ')));
    assert.equal(shadow.active_dialog?.label,'Confirm action');
    assert.match(shadow.text,/^Confirm action\nReview the requested operation/);
    assert(shadow.elements.some(e=>e.label==='Cancel operation'));
    assert(!shadow.dialogs.some(d=>d.label==='Hidden permission'));
    assert.equal(nested.active_dialog?.label,'Inner dialog');
    assert(nested.elements.some(e=>e.label==='Inner action'));
    assert(!nested.elements.some(e=>e.label==='Outer action'));
    assert.equal(nonmodal.active_dialog,null);assert(nonmodal.dialogs.some(d=>d.label==='Nonmodal panel'&&!d.modal));
    assert(nonmodal.elements.some(e=>e.label.startsWith('Background ')));
    await t.test('a modal beyond the reported-dialog cap stays selectable',()=>{
      assert.equal(overflow.dialogs.length,8);assert.equal(overflow.dialogs_truncated,true);
      assert.equal(overflow.active_dialog?.label,'Overflow modal');assert(overflow.elements.some(e=>e.label==='Close overflow'));
    });
    await t.test('a covering modal takes priority over focus behind it',()=>{
      assert.equal(covered.active_dialog?.label,'Covering modal');
      assert(covered.elements.some(e=>e.label==='Upper action'));assert(!covered.elements.some(e=>e.label==='Lower action'));
    });
    assert.equal(bounded.dialogs_truncated,true,'discovery must report its element bound');
    for(const r of output.snapshots){assert(r.elements.length<=180);assert(r.text.length<=12000);}
  } finally {
    // Delete only the directory created above, never a caller-supplied profile.
    assert.equal(dirname(resolve(directory)),parent);assert(basename(directory).startsWith('devbox-dialog-fixture-'));assert(!lstatSync(directory).isSymbolicLink());
    rmSync(directory,{recursive:true,force:true,maxRetries:5,retryDelay:100});
  }
});
