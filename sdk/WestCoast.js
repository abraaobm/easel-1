/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Web-only West Coast UI. Injected into the FM1 simulator page.
 * No changes to hardware screen/panel code. */
(() => {
  'use strict';
  if (typeof document === 'undefined') return;
  const $ = id => document.getElementById(id);

  const PAGE_NAMES = ['COMPLEX OSC', 'FUNCTION GEN', 'LOW PASS GATE', 'RANDOM'];
  const PAGES = [
    { label: 'OSC',  knobs: [
      { name: 'FOLD',  fmt: v => String(v), max: 99 },
      { name: 'RATIO', fmt: v => String(v), max: 31 },
      { name: 'FINE',  fmt: v => String(v), max: 99 },
      { name: 'FDBK',  fmt: v => String(v), max: 7  },
    ]},
    { label: 'FUNC', knobs: [
      { name: 'RISE',  fmt: v => v + '%', max: 99 },
      { name: 'FALL',  fmt: v => v + '%', max: 99 },
      { name: 'LOOP',  fmt: v => v ? 'ON' : 'OFF', max: 1 },
      { name: 'SHAPE', fmt: v => v + '%', max: 99 },
    ]},
    { label: 'LPG',  knobs: [
      { name: 'DEPTH',  fmt: v => v + '%', max: 99 },
      { name: 'OFFSET', fmt: v => String(v), max: 127 },
      { name: 'SOFT',   fmt: v => v + '%', max: 99 },
      { name: 'RESP',   fmt: v => v + '%', max: 99 },
    ]},
    { label: 'RAND', knobs: [
      { name: 'PITCH',  fmt: v => String(v), max: 99 },
      { name: 'RATE',   fmt: v => String(v), max: 99 },
      { name: 'WANDER', fmt: v => String(v), max: 99 },
      { name: 'WAVE',   fmt: v => ['TRI','SAW-','SAW+','SQR','SINE','S/H'][v] || String(v), max: 5 },
    ]},
  ];

  const state = { track: 0, page: 0, values: new Uint8Array(16), visible: false, api: null };

  function waitForApi() {
    return new Promise(resolve => {
      const check = () => globalThis.FM1WC ? resolve(globalThis.FM1WC) : setTimeout(check, 120);
      check();
    });
  }

  function injectStyles() {
    if ($('sim-wc-styles')) return;
    const style = document.createElement('style');
    style.id = 'sim-wc-styles';
    style.textContent = `
      .wc-panel{position:absolute;right:1.5%;top:2.5%;width:42%;max-width:340px;
        background:#16222099;backdrop-filter:blur(6px);
        border:1px solid #6bd4a3;border-radius:8px;padding:12px;z-index:30;
        color:#d4e8dc;font:12px/1.4 system-ui,sans-serif;box-shadow:0 10px 30px #000a}
      .wc-panel[hidden]{display:none}
      .wc-header{display:flex;align-items:center;gap:8px;
        border-bottom:1px solid #2f4840;padding-bottom:6px;margin-bottom:8px}
      .wc-header strong{color:#9df0c7;letter-spacing:.08em;font-size:11px}
      .wc-header #wc-track-label{color:#8aac9d;font-size:10px}
      .wc-header #wc-page-label{color:#d4e8dc;font-size:10px;margin-left:auto}
      .wc-header #wc-close{background:none;border:none;color:#8aac9d;
        font-size:16px;cursor:pointer;padding:0 4px;line-height:1}
      .wc-header #wc-close:hover{color:#ffb0a0}
      .wc-page-nav{display:flex;gap:4px;margin-bottom:8px}
      .wc-page-nav button{flex:1;padding:4px 6px;background:#243632;color:#b8d4c5;
        border:1px solid #3a554a;border-radius:4px;cursor:pointer;font-size:10px}
      .wc-page-nav button[aria-pressed=true]{background:#4a8060;color:#e8ffe8;border-color:#9df0c7}
      .wc-knobs{display:grid;grid-template-columns:1fr 1fr;gap:6px}
      .wc-knob{background:#243632;border:1px solid #3a554a;border-radius:6px;
        padding:6px 8px;text-align:center;user-select:none;touch-action:none;cursor:ns-resize}
      .wc-knob label{display:block;font-size:9px;color:#8aac9d;margin-bottom:2px;letter-spacing:.04em}
      .wc-knob .wc-val{font:18px/1.1 Consolas,monospace;color:#9df0c7}
      .wc-knob .wc-valbar{height:2px;background:#1a2624;margin-top:4px;border-radius:2px;overflow:hidden}
      .wc-knob .wc-valbar>i{display:block;height:100%;background:#6bd4a3;transition:width .05s}
      .wc-knob.drag{background:#2e4640;border-color:#6bd4a3}
      .wc-footer{display:flex;gap:6px;margin-top:10px}
      .wc-footer button{flex:1;padding:6px 8px;background:#243632;color:#b8d4c5;
        border:1px solid #3a554a;border-radius:4px;cursor:pointer;font-size:10px}
      .wc-footer button:hover{background:#2f4640}
    `;
    document.head.appendChild(style);
  }

  function buildPanel() {
    if ($('sim-wc-panel')) return $('sim-wc-panel');
    const device = $('sim-device');
    if (!device) return null;
    injectStyles();
    const panel = document.createElement('div');
    panel.id = 'sim-wc-panel';
    panel.className = 'wc-panel';
    panel.hidden = true;
    panel.innerHTML = `
      <div class="wc-header">
        <strong>WEST COAST</strong>
        <span id="wc-track-label">T1</span>
        <span id="wc-page-label">COMPLEX OSC</span>
        <button id="wc-close" aria-label="Close">×</button>
      </div>
      <div class="wc-page-nav">
        ${PAGES.map((p,i) => `<button data-wc-page="${i}" aria-pressed="${i===0}">${p.label}</button>`).join('')}
      </div>
      <div class="wc-knobs">
        ${[0,1,2,3].map(i => `
          <div class="wc-knob" data-wc-knob="${i}">
            <label>${PAGES[0].knobs[i].name}</label>
            <div class="wc-val">0</div>
            <div class="wc-valbar"><i style="width:0"></i></div>
          </div>`).join('')}
      </div>
      <div class="wc-footer">
        <button id="wc-reset">Reset Patch</button>
        <button id="wc-exit">Exit</button>
      </div>
    `;
    device.appendChild(panel);
    return panel;
  }

  function addWCButton() {
    if ($('sim-wc-open')) return $('sim-wc-open');
    const shortcuts = document.querySelector('#tab-sim .edit-shortcuts');
    if (!shortcuts) return null;
    const btn = document.createElement('button');
    btn.id = 'sim-wc-open';
    btn.type = 'button';
    btn.textContent = 'West Coast';
    shortcuts.appendChild(btn);
    return btn;
  }

  function render() {
    const panel = $('sim-wc-panel');
    if (!panel) return;
    const p = PAGES[state.page];
    $('wc-track-label').textContent = 'T' + (state.track + 1);
    $('wc-page-label').textContent = PAGE_NAMES[state.page];
    panel.querySelectorAll('[data-wc-page]').forEach(b => {
      b.setAttribute('aria-pressed', String(Number(b.dataset.wcPage) === state.page));
    });
    panel.querySelectorAll('.wc-knob').forEach(el => {
      const i = Number(el.dataset.wcKnob);
      const def = p.knobs[i];
      const raw = state.values[state.page * 4 + i];
      el.querySelector('label').textContent = def.name;
      el.querySelector('.wc-val').textContent = def.fmt(raw);
      const pct = Math.min(100, Math.round(raw / def.max * 100));
      el.querySelector('.wc-valbar > i').style.width = pct + '%';
    });
  }

  async function refreshValues() {
    if (!state.api) return;
    try {
      state.values = await state.api.read(state.track);
      render();
    } catch (e) { console.warn('[WC] read failed:', e); }
  }

  let deltaBuf = 0, deltaTimer = 0;
  async function flushDelta(knob) {
    const d = deltaBuf; deltaBuf = 0; deltaTimer = 0;
    if (!state.api || !d) return;
    try { await state.api.adjust(state.track, knob, d); await refreshValues(); }
    catch (e) { console.warn('[WC] adjust failed:', e); }
  }
  function queueDelta(knob, delta) {
    deltaBuf += delta;
    if (!deltaTimer) deltaTimer = setTimeout(() => flushDelta(knob), 40);
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
      if (d) { moved = true; lastY -= d * 4; queueDelta(knob, d); }
    });
    const up = e => {
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      el.classList.remove('drag');
      if (!moved) queueDelta(knob, 1);
    };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('wheel', e => {
      if (!e.deltaY || e.ctrlKey || e.metaKey) return;
      e.preventDefault();
      queueDelta(knob, e.deltaY < 0 ? 1 : -1);
    }, {passive: false});
  }

  function wirePanel(panel) {
    if (panel.dataset.wired) return;
    panel.dataset.wired = '1';
    $('wc-close').onclick = $('wc-exit').onclick = closePanel;
    panel.querySelectorAll('[data-wc-page]').forEach(btn => {
      btn.onclick = () => {
        state.page = Number(btn.dataset.wcPage);
        if (state.api) state.api.setPage(state.track, state.page);
        render();
      };
    });
    $('wc-reset').onclick = async () => {
      if (!state.api) return;
      if (!confirm('Reinstall WC INIT patch on this track? Current edits will be lost.')) return;
      await state.api.reset(state.track);
      await refreshValues();
    };
    panel.querySelectorAll('.wc-knob').forEach(el => {
      wireKnob(el, Number(el.dataset.wcKnob));
    });
  }

  async function openPanel() {
    const api = await waitForApi();
    state.api = api;
    // Determine current track from state
    state.track = await api.currentTrack();
    try { await api.enter(state.track); }
    catch (e) {
      alert('West Coast needs a DX7 track.\nSelect a DX7 track and try again.\n\n' + e.message);
      return;
    }
    const panel = buildPanel();
    if (!panel) return;
    state.page = await api.page(state.track);
    state.visible = true;
    panel.hidden = false;
    wirePanel(panel);
    await refreshValues();
  }

  async function closePanel() {
    const panel = $('sim-wc-panel');
    if (panel) panel.hidden = true;
    state.visible = false;
    if (state.api) { try { await state.api.leave(state.track); } catch (_) {} }
  }

  async function main() {
    const btn = addWCButton();
    if (!btn) { setTimeout(main, 300); return; }
    btn.disabled = true;
    const api = await waitForApi();
    btn.disabled = false;
    btn.onclick = openPanel;
    // Close panel if external track change breaks WC session
    setInterval(async () => {
      if (!state.visible || !state.api) return;
      try {
        const t = await state.api.currentTrack();
        if (t !== state.track) {
          await state.api.leave(state.track);
          state.visible = false;
          const panel = $('sim-wc-panel');
          if (panel) panel.hidden = true;
          console.info('[WC] track changed to T' + (t + 1) + ', panel closed');
        }
      } catch (_) {}
    }, 500);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', main);
  else main();
})();