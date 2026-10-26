/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
(() => {
  'use strict';
  if (typeof document === 'undefined') return;
  const $ = id => document.getElementById(id);

  const wc = { visible: false, api: null, page: 0, state: [] };

  const PAGES = [
    { name: 'VOICE', knobs: [
      { label: 'SHAPE', type: 'cont', def: 0 },
      { label: 'FOLD', type: 'cont', def: 20 },
      { label: 'DECAY', type: 'cont', def: 50 },
      { label: 'LPG CUTOFF', type: 'cont', def: 50 }
    ]},
    { name: 'MOTION', knobs: [
      { label: 'RISE', type: 'cont', def: 10 },
      { label: 'FALL', type: 'cont', def: 30 },
      { label: 'LOOP', type: 'switch3', def: 0, labels: ['OFF', 'LFO', 'EUCLID'] },
      { label: 'ROUTE', type: 'switch', def: 0, labels: ['FOLD', 'PITCH'] }
    ]},
    { name: 'MOD/RAND', knobs: [
      { label: 'RATE', type: 'cont', def: 50 },
      { label: 'AMOUNT', type: 'cont', def: 0 },
      { label: 'TARGET', type: 'switch', def: 0, labels: ['TIMBRE', 'PITCH'] },
      { label: 'SMOOTH', type: 'cont', def: 0 }
    ]},
    { name: 'SPACE', knobs: [
      { label: 'P. AMOUNT', type: 'cont', def: 50 },
      { label: 'P. DECAY', type: 'cont', def: 20 },
      { label: 'REV MIX', type: 'cont', def: 30 },
      { label: 'REV TIME', type: 'cont', def: 50 }
    ]},
    { name: 'SEQUENCE', knobs: [
      { label: 'TRANSPOSE', type: 'cont', def: 50 },
      { label: 'OCTAVE', type: 'cont', def: 50 },
      { label: 'SCALE', type: 'switch', def: 0, labels: ['FREE', 'PENTA'] },
      { label: 'TIMBRE AMT', type: 'cont', def: 50 }
    ]}
  ];

  for (let p = 0; p < PAGES.length; p++) {
    wc.state[p] = PAGES[p].knobs.map(k => k.def);
  }

  function waitForApi() {
    return new Promise(resolve => {
      const check = () => globalThis.FM1Easel ? resolve(globalThis.FM1Easel) : setTimeout(check, 120);
      check();
    });
  }

  function injectStyles() {
    if ($('sim-easel-styles')) return;
    const style = document.createElement('style');
    style.id = 'sim-easel-styles';
    style.textContent = `
      .easel-panel{position:absolute;right:1.5%;top:2.5%;width:42%;max-width:340px;
        background:#1a1c18f2;backdrop-filter:blur(8px);
        border:2px solid #8bc34a;border-radius:8px;padding:12px;z-index:30;
        color:#dcedc8;font:12px/1.4 system-ui,sans-serif;box-shadow:0 10px 40px #000c}
      .easel-panel[hidden]{display:none}
      .easel-header{display:flex;align-items:center;gap:8px;
        border-bottom:1px solid #33691e;padding-bottom:6px;margin-bottom:10px}
      .easel-header strong{color:#8bc34a;letter-spacing:.1em;font-size:12px;text-shadow:0 0 5px #8bc34a50}
      .easel-header span { color:#689f38; font-size:10px; margin-left:4px; }
      .easel-header #easel-close{background:none;border:none;color:#aed581;
        font-size:18px;cursor:pointer;padding:0 4px;line-height:1;margin-left:auto}
      .easel-header #easel-close:hover{color:#ff8a65}
      .easel-nav{display:flex;gap:4px;margin-bottom:10px}
      .easel-nav button{flex:1;padding:6px;background:#2e3b25;color:#aed581;
        border:1px solid #33691e;border-radius:4px;cursor:pointer;font-size:9px;font-weight:bold}
      .easel-nav button[aria-pressed=true]{background:#558b2f;color:#fff;border-color:#8bc34a}
      .easel-knobs{display:grid;grid-template-columns:1fr 1fr;gap:8px}
      .easel-knob{background:#263238;border:1px solid #455a64;border-radius:6px;
        padding:8px;text-align:center;user-select:none;touch-action:none;cursor:ns-resize;
        box-shadow:inset 0 0 10px #0006}
      .easel-knob label{display:block;font-size:9px;color:#90a4ae;margin-bottom:2px;letter-spacing:.05em}
      .easel-knob .easel-val{font:18px/1.1 Consolas,monospace;color:#8bc34a}
      .easel-knob .easel-valbar{height:2px;background:#1a1c18;margin-top:6px;border-radius:2px;overflow:hidden}
      .easel-knob .easel-valbar>i{display:block;height:100%;background:#8bc34a;transition:width .05s}
      .easel-knob.drag{background:#37474f;border-color:#8bc34a}
      .easel-footer{display:flex;gap:6px;margin-top:12px}
      .easel-footer button{flex:1;padding:8px;background:#2e3b25;color:#aed581;
        border:1px solid #33691e;border-radius:4px;cursor:pointer;font-size:10px;font-weight:bold;}
      .easel-footer button:hover{background:#558b2f;color:#fff}
      .easel-footer button.test { background:#8bc34a; color:#1a1c18; border-color:#dcedc8;}
      .easel-footer button.test:hover { background:#aed581; }
    `;
    document.head.appendChild(style);
  }

  function buildPanel() {
    if ($('sim-easel-panel')) return $('sim-easel-panel');
    const device = $('sim-device');
    if (!device) return null;
    injectStyles();
    const panel = document.createElement('div');
    panel.id = 'sim-easel-panel';
    panel.className = 'easel-panel';
    panel.hidden = true;
    panel.innerHTML = `
      <div class="easel-header">
        <strong>EASEL-1 <span>v2.0</span></strong>
        <button id="easel-close" aria-label="Close">×</button>
      </div>
      <div class="easel-nav">
        ${PAGES.map((p, i) => `<button data-easel-page="${i}" aria-pressed="${i===0}">${p.name}</button>`).join('')}
      </div>
      <div class="easel-knobs">
        ${[0,1,2,3].map(i => `
          <div class="easel-knob" data-easel-knob="${i}">
            <label>${PAGES[0].knobs[i].label}</label><div class="easel-val">${PAGES[0].knobs[i].def}</div>
            <div class="easel-valbar"><i style="width:${PAGES[0].knobs[i].def}%"></i></div>
          </div>`).join('')}
      </div>
      <div class="easel-footer">
        <button id="easel-test" class="test">TEST NOTE</button>
        <button id="easel-exit">Exit</button>
      </div>
    `;
    device.appendChild(panel);
    return panel;
  }

  function addEaselButton() {
    if ($('sim-easel-open')) return $('sim-easel-open');
    const shortcuts = document.querySelector('#tab-sim .edit-shortcuts');
    if (!shortcuts) return null;
    const btn = document.createElement('button');
    btn.id = 'sim-easel-open';
    btn.type = 'button';
    btn.textContent = 'Easel-1';
    btn.style.background = '#33691e';
    btn.style.borderColor = '#8bc34a';
    shortcuts.appendChild(btn);
    return btn;
  }

  function updateKnobUI(el, page, knobIdx) {
    const def = PAGES[page].knobs[knobIdx];
    const val = wc.state[page][knobIdx];
    
    if (page === 1) {
        if (knobIdx === 0 && wc.state[1][2] > 66) el.querySelector('label').textContent = 'CLOCK RATE';
        else if (knobIdx === 1 && wc.state[1][2] > 66) el.querySelector('label').textContent = 'PULSES';
        else el.querySelector('label').textContent = def.label;
    } else {
        el.querySelector('label').textContent = def.label;
    }

    if (def.type === 'switch') {
      el.querySelector('.easel-val').textContent = def.labels[val > 50 ? 1 : 0];
      el.querySelector('.easel-valbar > i').style.width = (val > 50 ? 100 : 0) + '%';
    } else if (def.type === 'switch3') {
      let idx = val < 25 ? 0 : val < 75 ? 1 : 2;
      el.querySelector('.easel-val').textContent = def.labels[idx];
      el.querySelector('.easel-valbar > i').style.width = (idx * 50) + '%';
    } else {
      // No modo TRANSPOSE, mostra o valor real em semitons
      if (page === 4 && knobIdx === 0) {
          let v = Math.round(val - 50);
          el.querySelector('.easel-val').textContent = (v > 0 ? '+' : '') + v;
      } else if (page === 4 && knobIdx === 1) {
          let v = Math.round((val - 50) / 25);
          el.querySelector('.easel-val').textContent = (v > 0 ? '+' : '') + v;
      } else {
          el.querySelector('.easel-val').textContent = Math.round(val);
      }
      el.querySelector('.easel-valbar > i').style.width = val + '%';
    }
  }

  function renderPage(page) {
    wc.page = page;
    const panel = $('sim-easel-panel');
    panel.querySelectorAll('[data-easel-page]').forEach(b => {
      b.setAttribute('aria-pressed', String(Number(b.dataset.easelPage) === page));
    });
    panel.querySelectorAll('.easel-knob').forEach(el => {
      updateKnobUI(el, page, Number(el.dataset.easelKnob));
    });
  }

  function applyValue(el, page, knobIdx, val) {
    wc.state[page][knobIdx] = val;
    updateKnobUI(el, page, knobIdx);

    if (globalThis.FM1Easel) {
        let paramIdx = page === 4 ? 18 + knobIdx : page * 4 + knobIdx;

        if (page === 1 && knobIdx === 2) {
            globalThis.FM1Easel.setParam(6, val / 100);
            updateKnobUI(el.parentElement.querySelector('[data-easel-knob="0"]'), 1, 0);
            updateKnobUI(el.parentElement.querySelector('[data-easel-knob="1"]'), 1, 1);
            if (val > 66) {
                globalThis.FM1Easel.setParam(16, wc.state[1][0] / 100);
                globalThis.FM1Easel.setParam(17, wc.state[1][1] / 100);
            } else {
                globalThis.FM1Easel.setParam(4, wc.state[1][0] / 100);
                globalThis.FM1Easel.setParam(5, wc.state[1][1] / 100);
            }
        } else if (page === 1 && knobIdx === 0) {
            globalThis.FM1Easel.setParam(wc.state[1][2] > 66 ? 16 : 4, val / 100);
        } else if (page === 1 && knobIdx === 1) {
            globalThis.FM1Easel.setParam(wc.state[1][2] > 66 ? 17 : 5, val / 100);
        } else {
            globalThis.FM1Easel.setParam(paramIdx, val / 100);
        }
    }
  }

  function wireKnob(el, knobIdx) {
    let lastY = 0, moved = false;
    const def = PAGES[wc.page].knobs[knobIdx];
    
    el.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
      e.preventDefault();
      lastY = e.clientY; moved = false;
      el.classList.add('drag');
      el.setPointerCapture(e.pointerId);
    });
    
    el.addEventListener('pointermove', e => {
      if (!el.hasPointerCapture(e.pointerId)) return;
      const d = Math.trunc((lastY - e.clientY) / 4);
      if (d) {
        moved = true; lastY -= d * 4;
        const current = wc.state[wc.page][knobIdx];
        let next = current;
        
        if (def.type === 'switch') {
            next = current > 50 ? 0 : 100;
        } else if (def.type === 'switch3') {
            if (current < 25) next = 50;
            else if (current < 75) next = 100;
            else next = 0;
        } else {
            next = Math.max(0, Math.min(100, current + d));
        }
        applyValue(el, wc.page, knobIdx, next);
      }
    });
    
    const up = e => {
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      el.classList.remove('drag');
      
      if (!moved && def.type !== 'cont') {
          const current = wc.state[wc.page][knobIdx];
          let next;
          if (def.type === 'switch') next = current > 50 ? 0 : 100;
          else if (def.type === 'switch3') {
              if (current < 25) next = 50;
              else if (current < 75) next = 100;
              else next = 0;
          }
          applyValue(el, wc.page, knobIdx, next);
      }
    };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
  }

  async function openPanel() {
    const api = await waitForApi();
    wc.api = api;
    
    const startBtn = document.getElementById('sim-start');
    if (startBtn && startBtn.getAttribute('aria-pressed') === 'false') startBtn.click();

    const panel = buildPanel();
    if (!panel) return;
    wc.visible = true;
    panel.hidden = false;
    
    if (!panel.dataset.wired) {
      panel.dataset.wired = '1';
      $('easel-close').onclick = $('easel-exit').onclick = () => { panel.hidden = true; };
      $('easel-test').onclick = () => {
          if (globalThis.FM1Easel) globalThis.FM1Easel.note(60, 127);
          setTimeout(() => { if (globalThis.FM1Easel) globalThis.FM1Easel.note(60, 0); }, 2000);
      };
      
      panel.querySelectorAll('[data-easel-page]').forEach(btn => {
        btn.onclick = () => renderPage(Number(btn.dataset.easelPage));
      });
      
      panel.querySelectorAll('.easel-knob').forEach(el => {
        wireKnob(el, Number(el.dataset.easelKnob));
      });
      
      for (let p = 0; p < PAGES.length; p++) {
        for (let k = 0; k < 4; k++) {
          if (globalThis.FM1Easel) {
              let paramIdx = p === 4 ? 18 + k : p * 4 + k;
              globalThis.FM1Easel.setParam(paramIdx, wc.state[p][k] / 100);
          }
        }
      }
    }
    renderPage(wc.page);
  }

  async function main() {
    const btn = addEaselButton();
    if (!btn) { setTimeout(main, 300); return; }
    btn.disabled = true;
    await waitForApi();
    btn.disabled = false;
    btn.onclick = openPanel;
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', main);
  else main();
})();
