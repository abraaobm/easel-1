/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
(() => {
  'use strict';
  if (typeof document === 'undefined') return;
  const $ = id => document.getElementById(id);

  const wc = { visible: false, api: null };

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
      .easel-header #easel-close{background:none;border:none;color:#aed581;
        font-size:18px;cursor:pointer;padding:0 4px;line-height:1;margin-left:auto}
      .easel-header #easel-close:hover{color:#ff8a65}
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
        <strong>EASEL-1 SYNTH</strong>
        <button id="easel-close" aria-label="Close">×</button>
      </div>
      <div class="easel-knobs">
        <div class="easel-knob" data-easel-knob="0">
          <label>SHAPE</label><div class="easel-val">0</div>
          <div class="easel-valbar"><i style="width:0"></i></div>
        </div>
        <div class="easel-knob" data-easel-knob="1">
          <label>FOLD</label><div class="easel-val">20</div>
          <div class="easel-valbar"><i style="width:20%"></i></div>
        </div>
        <div class="easel-knob" data-easel-knob="2">
          <label>DECAY</label><div class="easel-val">50</div>
          <div class="easel-valbar"><i style="width:50%"></i></div>
        </div>
        <div class="easel-knob" data-easel-knob="3">
          <label>LPG CUTOFF</label><div class="easel-val">50</div>
          <div class="easel-valbar"><i style="width:50%"></i></div>
        </div>
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

  function render(knob, val) {
    const panel = $('sim-easel-panel');
    if (!panel) return;
    const el = panel.querySelector(`[data-easel-knob="${knob}"]`);
    if (!el) return;
    el.querySelector('.easel-val').textContent = Math.round(val * 100);
    el.querySelector('.easel-valbar > i').style.width = (val * 100) + '%';
  }

  function wireKnob(el, knob) {
    let lastY = 0, moved = false;
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
        const current = parseFloat(el.querySelector('.easel-val').textContent) / 100;
        const next = Math.max(0, Math.min(1, current + (d * 0.01)));
        if (globalThis.FM1Easel) globalThis.FM1Easel.setParam(knob, next);
        render(knob, next);
      }
    });
    const up = e => {
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      el.classList.remove('drag');
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
          // Toca um Dó central (60) com velocidade máxima
          if (globalThis.FM1Easel) globalThis.FM1Easel.note(60, 127);
          // Solta a nota após 2 segundos
          setTimeout(() => { if (globalThis.FM1Easel) globalThis.FM1Easel.note(60, 0); }, 2000);
      };
      panel.querySelectorAll('.easel-knob').forEach(el => {
        wireKnob(el, Number(el.dataset.easelKnob));
      });
      
      // Envia os valores iniciais para o motor C
      globalThis.FM1Easel.setParam(0, 0.0);
      globalThis.FM1Easel.setParam(1, 0.2);
      globalThis.FM1Easel.setParam(2, 0.5);
      globalThis.FM1Easel.setParam(3, 0.5);
    }
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
