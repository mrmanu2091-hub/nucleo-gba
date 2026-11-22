/* Aplica el parche del cable Link al código fuente de mGBA-wasm.  node aplicar-parche.js <carpeta-del-repo>
   Copia netlink.* y modifica main.c y CMakeLists.txt de src/platform/wasm (si ya está aplicado, no hace nada). */
const fs = require('fs'), path = require('path');
const repo = path.resolve(process.argv[2] || '.'), wasm = path.join(repo, 'src', 'platform', 'wasm');
const die = (m) => { console.error('ERROR: ' + m); process.exit(1); };
if (!fs.existsSync(path.join(wasm, 'main.c'))) die('no encuentro ' + path.join(wasm, 'main.c'));
['netlink.c', 'netlink.h', 'netlink_engine.h'].forEach(f => fs.copyFileSync(path.join(__dirname, f), path.join(wasm, f)));

function patch(file, edits) {
  let s = fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
  edits.forEach(([name, anchor, repl]) => {
    if (s.includes(repl.marker)) { console.log('  ya aplicado: ' + name); return; }
    const n = s.split(anchor).length - 1;
    if (n !== 1) die('el ancla «' + name + '» aparece ' + n + ' veces en ' + path.basename(file) + ' (esperaba 1). Esta versión del núcleo no coincide: súbeme info-fuente.txt otra vez.');
    s = s.replace(anchor, () => repl.text); console.log('  parche aplicado: ' + name);
  });
  fs.writeFileSync(file, s, 'utf8');
}
patch(path.join(wasm, 'main.c'), [
  ['include', '#include "main.h"\n', { marker: '#include "netlink.h"', text: '#include "main.h"\n#include "netlink.h"\n' }],
  ['enganche en loadGame', '\trenderer->core->reset(renderer->core);\n\n\tunsigned w, h;\n\trenderer->core->currentVideoSize(renderer->core, &w, &h);\n\tmGLES2Init(renderer, w, h);',
    { marker: 'netlinkAttach(renderer->core);', text: '\trenderer->core->reset(renderer->core);\n\tnetlinkAttach(renderer->core); /* GBA 2091: cable Link por red (solo si está armado desde JS) */\n\n\tunsigned w, h;\n\trenderer->core->currentVideoSize(renderer->core, &w, &h);\n\tmGLES2Init(renderer, w, h);' }]
]);
patch(path.join(wasm, 'CMakeLists.txt'), [
  ['netlink.c en CMake', 'main.c memory.c', { marker: 'main.c netlink.c memory.c', text: 'main.c netlink.c memory.c' }]
]);
console.log('Parche listo.');
