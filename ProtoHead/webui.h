#pragma once
// Page d'upload des faces, servie par la tête quand le mode upload est actif.
// La conversion PNG -> calques 1 bit se fait dans le navigateur (le même algo que
// tools/png2layers.py, garde les réglages identiques), l'ESP32 ne reçoit que le résultat.

static const char WEB_UI[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ProtoBase</title>
<style>
:root{--bg:#121216;--card:#1d1d23;--fg:#e8e8ee;--dim:#9a9aa6;--acc:#b07cff;--err:#ff6b6b;--ok:#5be39a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,sans-serif;padding:16px;max-width:640px;margin:auto}
h1{font-size:22px;margin:4px 0 16px}
.card{background:var(--card);border-radius:12px;padding:14px;margin-bottom:14px}
label{display:block;color:var(--dim);font-size:14px;margin-bottom:6px}
select,input[type=file]{width:100%;padding:10px;border-radius:8px;border:1px solid #333;background:#26262e;color:var(--fg);font-size:16px}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
button{flex:1;min-width:120px;padding:12px;border:0;border-radius:8px;background:#2c2c36;color:var(--fg);font-size:15px}
button.main{background:var(--acc);color:#120a1f;font-weight:600}
button:disabled{opacity:.4}
canvas{width:100%;image-rendering:pixelated;background:#000;border-radius:6px;display:block}
#status{min-height:1.4em;font-size:14px;color:var(--dim)}
#status.err{color:var(--err)}#status.ok{color:var(--ok)}
.hint{font-size:13px;color:var(--dim);margin-top:8px}
</style></head><body>
<h1>ProtoBase · faces</h1>

<div class="card">
  <label for="slot">Face à remplacer</label>
  <select id="slot"></select>
  <div class="row">
    <button id="show">Afficher sur la visière</button>
    <button id="reset">Remettre d'origine</button>
  </div>
</div>

<div class="card">
  <label for="files">PNG de la nouvelle face</label>
  <input id="files" type="file" accept="image/png" multiple>
  <p class="hint">Image au ratio 2:1 (ex. 2000×1000) = une dalle, affichée en miroir sur l'autre.
  Plusieurs PNG = étapes du clignement, de l'œil ouvert à l'œil fermé (triés par nom, 4 max).
  La bouche et le nez sont pris sur le premier.</p>
  <canvas id="preview" width="512" height="128"></canvas>
  <div class="row"><button id="send" class="main" disabled>Envoyer sur la visière</button></div>
</div>

<div id="status"></div>

<script>
// ⚠ Mêmes réglages que tools/png2layers.py
const W=64,H=32,LUM_MIN=60,COVERAGE=0.12,SPLIT_Y=16,NOSE_X=56,MAX_EYES=4;
const $=id=>document.getElementById(id);
let layers=null, anim=null, gen=0;

function status(msg,kind){const s=$('status');s.textContent=msg;s.className=kind||'';}

// RGBA -> masque 64x32 + couleur moyenne par cellule
function analyze(d,w,h){
  const cnt=new Float64Array(W*H), sum=new Float64Array(W*H*3);
  for(let y=0;y<h;y++){
    const cy=Math.floor(y*H/h);
    for(let x=0;x<w;x++){
      const i=(y*w+x)*4, l=Math.max(d[i],d[i+1],d[i+2])*d[i+3]/255;
      if(l>LUM_MIN){const k=cy*W+Math.floor(x*W/w);cnt[k]++;sum[k*3]+=d[i];sum[k*3+1]+=d[i+1];sum[k*3+2]+=d[i+2];}
    }
  }
  const area=(w/W)*(h/H), mask=new Uint8Array(W*H), col=new Float64Array(W*H*3);
  for(let k=0;k<W*H;k++){
    mask[k]=cnt[k]/area>COVERAGE?1:0;
    const n=Math.max(cnt[k],1);col[k*3]=sum[k*3]/n;col[k*3+1]=sum[k*3+1]/n;col[k*3+2]=sum[k*3+2]/n;
  }
  return {mask,col};
}

function inRegion(kind,x,y){
  if(kind==='eye') return y<SPLIT_Y&&x<NOSE_X;
  if(kind==='nose') return y<SPLIT_Y&&x>=NOSE_X;
  return y>=SPLIT_Y;
}

// Un calque : masque restreint à sa zone + dégradé gauche -> droite
function makeLayer(a,kind){
  const m=new Uint8Array(W*H);let x0=W,x1=-1;
  for(let y=0;y<H;y++)for(let x=0;x<W;x++){const k=y*W+x;
    if(a.mask[k]&&inRegion(kind,x,y)){m[k]=1;x0=Math.min(x0,x);x1=Math.max(x1,x);}}
  if(x1<0) return {m,x0:0,x1:0,c0:[0,0,0],c1:[0,0,0]};
  const span=Math.max(1,Math.floor((x1-x0+1)/4));
  const avg=(lo,hi)=>{const s=[0,0,0];let n=0;
    for(let y=0;y<H;y++)for(let x=lo;x<hi;x++){const k=y*W+x;if(m[k]){n++;for(let c=0;c<3;c++)s[c]+=a.col[k*3+c];}}
    return s.map(v=>Math.round(v/n));};
  return {m,x0,x1,c0:avg(x0,x0+span),c1:avg(x1-span+1,x1+1)};
}

// Pixels bruts du PNG, sans correction de couleur du navigateur (comme PIL côté Python)
async function loadImage(file){
  if(window.createImageBitmap){
    try{return await createImageBitmap(file,{colorSpaceConversion:'none',premultiplyAlpha:'none'});}catch(e){}
  }
  const url=URL.createObjectURL(file);
  try{return await new Promise((ok,ko)=>{const img=new Image();img.onload=()=>ok(img);img.onerror=()=>ko(file.name);img.src=url;});}
  finally{URL.revokeObjectURL(url);}
}

async function buildLayers(files){
  const eyes=[];let mouth,nose;
  for(const [i,f] of files.entries()){
    const img=await loadImage(f), c=document.createElement('canvas');
    c.width=img.naturalWidth||img.width;c.height=img.naturalHeight||img.height;
    const g=c.getContext('2d');g.drawImage(img,0,0);
    const a=analyze(g.getImageData(0,0,c.width,c.height).data,c.width,c.height);
    eyes.push(makeLayer(a,'eye'));
    if(i===0){mouth=makeLayer(a,'mouth');nose=makeLayer(a,'nose');}
  }
  return {eyes,mouth,nose};
}

function drawPreview(L,eye){
  const cv=$('preview'),g=cv.getContext('2d'),s=cv.width/(W*2);
  g.fillStyle='#000';g.fillRect(0,0,cv.width,cv.height);
  for(const l of [L.eyes[eye],L.mouth,L.nose]){
    const span=Math.max(1,l.x1-l.x0);
    for(let y=0;y<H;y++)for(let x=0;x<W;x++){
      if(!l.m[y*W+x])continue;
      const t=Math.min(256,Math.max(0,Math.trunc((x-l.x0)*256/span)));
      g.fillStyle='rgb('+[0,1,2].map(c=>l.c0[c]+Math.trunc((l.c1[c]-l.c0[c])*t/256))+')';
      for(const xx of [x,2*W-1-x]) g.fillRect(xx*s+1,y*s+1,s-2,s-2);
    }
  }
}

// Format envoyé (en hex) : 'P','F',1,nbYeux puis pour chaque calque (yeux..., bouche, nez)
// x0,x1,c0[3],c1[3] + 256 octets de masque (MSB = pixel de gauche)
function encode(L){
  const all=[...L.eyes,L.mouth,L.nose], out=[0x50,0x46,1,L.eyes.length];
  for(const l of all){
    out.push(l.x0,l.x1,...l.c0,...l.c1);
    for(let i=0;i<W*H;i+=8){let b=0;for(let j=0;j<8;j++)b=(b<<1)|l.m[i+j];out.push(b);}
  }
  return out.map(b=>b.toString(16).padStart(2,'0')).join('');
}

function slot(){const v=$('slot').value;if(v==='')throw new Error('liste des faces pas chargée');return v;}
async function post(path,body){
  const r=await fetchRetry(path,{method:'POST',headers:{'Content-Type':'text/plain'},body:body||''},2);
  const t=await r.text();if(!r.ok)throw new Error(t||r.status);return t;
}

// fetch avec délai max + nouvelles tentatives (la radio de l'ESP32 peut perdre un paquet)
async function fetchRetry(url,opts,tries=4){
  for(let i=1;;i++){
    const ctl=new AbortController(), t=setTimeout(()=>ctl.abort(),5000);
    try{const r=await fetch(url,{...opts,signal:ctl.signal,cache:'no-store'});clearTimeout(t);return r;}
    catch(e){clearTimeout(t);if(i>=tries)throw e;status('Nouvel essai ('+i+'/'+(tries-1)+')…');await new Promise(ok=>setTimeout(ok,500*i));}
  }
}

async function refresh(quiet){
  try{
    const r=await (await fetchRetry('/faces')).json(), sel=$('slot'), cur=sel.value;
    sel.innerHTML='';
    for(const f of r.faces){const o=document.createElement('option');o.value=f.id;
      o.textContent=f.name+(f.custom?' (perso)':'');sel.appendChild(o);}
    if(cur)sel.value=cur;
    if(!quiet)status('Connecté. Mémoire libre : '+Math.round(r.heap/1024)+' Ko'+(r.fs?'':' · stockage indisponible, pas de sauvegarde'));
  }catch(e){status('Visière injoignable : '+e.message,'err');}
}

$('files').onchange=async e=>{
  const files=[...e.target.files].sort((a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true}));
  const my=++gen;  // si on rechoisit des fichiers pendant une conversion, seule la dernière compte
  clearInterval(anim);layers=null;$('send').disabled=true;
  if(!files.length)return;
  if(files.length>MAX_EYES){status(MAX_EYES+' PNG maximum','err');return;}
  try{
    status('Conversion…');const L=await buildLayers(files);
    if(my!==gen)return;
    layers=L;let f=0;drawPreview(layers,0);
    if(layers.eyes.length>1)anim=setInterval(()=>{f=(f+1)%layers.eyes.length;drawPreview(layers,f);},400);
    $('send').disabled=false;status(files.length+' image(s) prête(s)');
  }catch(err){if(my===gen)status('Image illisible : '+err,'err');}
};

$('send').onclick=async()=>{
  try{status('Envoi…');await post('/face?id='+slot(),encode(layers));
    status('Face envoyée et affichée','ok');refresh(true);}catch(e){status('Échec : '+e.message,'err');}
};
$('show').onclick=async()=>{try{await post('/show?id='+slot());status('Affichée','ok');}catch(e){status('Échec : '+e.message,'err');}};
$('reset').onclick=async()=>{
  if(!confirm('Remettre cette face d\'origine ?'))return;
  try{await post('/reset?id='+slot());status('Face d\'origine remise','ok');refresh(true);}catch(e){status('Échec : '+e.message,'err');}
};
refresh();
</script></body></html>)HTML";
