// Jura Z10 brew card. Products and their parameters come from the machine's own J.O.E.
// profile (EF545.xml): only the chosen product's parameters are shown, starting at its
// factory values. Brew sends one 16-byte @TP: recipe to the ESPHome action.
//
// Recipe layout (makefu/jura-connect PROTOCOL.md 5.9): byte 0 = product code, parameter
// F<n> at byte n-1, byte 8 = 01, the rest 00 (= "not set", the machine keeps its own).
// Brewed on a Z10: F3 strength, F4 water, F5 milk, F6 foam, F7 temperature. Everything
// else is marked "test" in the UI.

const VERIFIED = new Set(['F3', 'F4', 'F5', 'F6', 'F7']);
// Sent at their factory value even when untouched: the firmware's own encoder always did.
const SEND_ALWAYS = new Set([...VERIFIED, 'F10', 'F11']);

const LABELS = {
  GRINDER_FREENESS: 'Grind', COFFEE_STRENGTH: 'Strength', WATER_AMOUNT: 'Water',
  TEMPERATURE: 'Temperature', MILK_AMOUNT: 'Milk', MILK_FOAM_AMOUNT: 'Milk foam',
  MILK_FOAM_TEMP: 'Foam temperature', MILK_TEMP: 'Milk temperature', BYPASS: 'Bypass',
  MILK_BREAK: 'Milk break', STROKE: 'Stroke',
};
// Status pill, styled like makefu/jura-connect-hass's card; any other state stays grey.
const PILL_ICON = { ready: 'mdi:check-circle-outline', blocked: 'mdi:alert-circle-outline', off: 'mdi:power', offline: 'mdi:cloud-off-outline' };
const UNITS = { WATER_AMOUNT: 'ml', BYPASS: 'ml', MILK_AMOUNT: 's', MILK_FOAM_AMOUNT: 's', MILK_BREAK: 's' };

// Preselections on a machine without IntakeF18 (the Z10 has none): J.O.E.'s AppProduct.c
// swaps in the double product's code or overwrites one byte. Never seen on a wire.
// fakesweetfoam has no payload there, so it is not offered.
const PRESELECT = {
  double: { label: 'Double' },
  xtrashot: { label: 'Extra shot', byte: 7, value: 0x02 },
  coldbrew: { label: 'Cold brew', byte: 6, value: 0x80, hides: ['F7'] },
  powder: { label: 'Ground coffee', byte: 2, value: 0x00, hides: ['F3', 'F2_H'] },
};

function parseProfile(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  return [...doc.getElementsByTagName('PRODUCT')]
    .filter((p) => p.getAttribute('Active') !== 'false')
    .map((p) => {
      const pre = p.getElementsByTagName('PRESELECTION')[0];
      const dbl = pre ? pre.getAttribute('double') || '00' : '00';
      return {
        code: parseInt(p.getAttribute('Code'), 16),
        name: p.getAttribute('Name'),
        double: parseInt(dbl, 16),
        pre: Object.keys(PRESELECT).filter((k) => (k === 'double' ? dbl !== '00' : pre && pre.getAttribute(k) === 'true')),
        params: [...p.children]
          .filter((e) => /^F\d+(_H|_\d)?$/.test(e.getAttribute('Argument') || ''))
          .map((e) => {
            const items = [...e.getElementsByTagName('ITEM')].map((i) => ({
              name: i.getAttribute('Name'), value: parseInt(i.getAttribute('Value'), 16),
            }));
            return {
              tag: e.localName,
              arg: e.getAttribute('Argument'),
              items,
              def: items.length ? parseInt(e.getAttribute('Default'), 16) : Number(e.getAttribute('Value')),
              min: Number(e.getAttribute('Min')),
              max: Number(e.getAttribute('Max')),
              step: Number(e.getAttribute('Step') || 1),
            };
          }),
      };
    });
}

// The rest (F8 stroke, sub-indexed F2_H / F14_1 / F14_2) go out only when changed. The
// sub-indexed encoding is a guess: _H items are already high-nibble values (10..50),
// _1 = high nibble, _2 = low nibble.
function buildRecipe(product, values, changed, pre) {
  const b = new Uint8Array(16);
  const hidden = (PRESELECT[pre] && PRESELECT[pre].hides) || [];
  b[0] = pre === 'double' ? product.double : product.code;
  b[8] = 0x01;
  for (const p of product.params) {
    const [, n, sub] = p.arg.match(/^F(\d+)(?:_(H|\d))?$/);
    const i = Number(n) - 1;
    if (i < 1 || i > 15 || i === 8 || hidden.includes(p.arg) || (!SEND_ALWAYS.has(p.arg) && !changed.has(p.arg))) continue;
    const v = values[p.arg];
    if (!sub) b[i] = p.tag === 'WATER_AMOUNT' || p.tag === 'BYPASS' ? v / 5 : v;
    else if (sub === 'H') b[i] |= v;
    else b[i] |= sub === '1' ? v << 4 : v;
  }
  if (PRESELECT[pre] && PRESELECT[pre].byte !== undefined) b[PRESELECT[pre].byte] = PRESELECT[pre].value;
  return [...b].map((x) => x.toString(16).padStart(2, '0')).join('').toUpperCase();
}

// navigator.clipboard only exists in a secure context (https), and some WebViews refuse it;
// HA over plain http (or the Companion app) then needs the old execCommand path.
function copyText(text) {
  const legacy = () => {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.cssText = 'position:fixed;opacity:0';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    ta.remove();
    return ok ? Promise.resolve() : Promise.reject(new Error('copy failed'));
  };
  return navigator.clipboard && window.isSecureContext ? navigator.clipboard.writeText(text).catch(legacy) : legacy();
}

class JuraZ10Card extends HTMLElement {
  setConfig(config) {
    this._config = {
      title: 'Jura',
      profile: '/local/jura/EF545.xml',
      action: 'esphome.jura_z10_brew',
      status: 'sensor.jura_status',
      connection: 'binary_sensor.jura_connection',
      ...config,
    };
    if (!this.shadowRoot) this.attachShadow({ mode: 'open' });
    this._products = null;
    fetch(this._config.profile)
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(`${this._config.profile}: HTTP ${r.status}`))))
      .then((t) => { this._products = parseProfile(t); this._select(0); })
      .catch((e) => { this._error = e.message; this._render(); });
  }

  set hass(hass) {
    this._hass = hass;
    this._updateStatus();
  }

  getCardSize() { return 7; }

  _select(index) {
    this._product = this._products[index];
    this._values = Object.fromEntries(this._product.params.map((p) => [p.arg, p.def]));
    this._changed = new Set();
    this._pre = '';
    this._message = '';
    this._render();
  }

  _online() {
    const c = this._hass && this._hass.states[this._config.connection];
    return !!c && c.state === 'on';
  }

  _updateStatus() {
    const root = this.shadowRoot;
    if (!root || !this._hass) return;
    const s = this._hass.states[this._config.status];
    const el = root.getElementById('status');
    if (el) {
      const state = this._online() ? (s ? s.state : '') : 'offline';
      el.className = `pill ${state}`;
      el.querySelector('ha-icon').setAttribute('icon', PILL_ICON[state] || 'mdi:help-circle-outline');
      el.querySelector('span').textContent = state ? state[0].toUpperCase() + state.slice(1) : '?';
    }
    const btn = root.getElementById('brew');
    if (btn) btn.disabled = !this._online() || (!!s && s.state === 'off');   // off = switch-off delay running
  }

  _render() {
    const root = this.shadowRoot;
    const esc = (t) => String(t).replace(/[&<>"]/g, (c) => `&#${c.charCodeAt(0)};`);
    let body;
    if (this._error) body = `<div class="msg">${esc(this._error)}</div>`;
    else if (!this._products) body = '<div class="msg">Loading profile…</div>';
    else {
      const p = this._product;
      const hidden = (PRESELECT[this._pre] && PRESELECT[this._pre].hides) || [];
      const rows = p.params.filter((q) => !hidden.includes(q.arg)).map((q) => {
        const label = `${LABELS[q.tag] || q.tag}${VERIFIED.has(q.arg) ? '' : ' <span class="test">test</span>'}`;
        const v = this._values[q.arg];
        const control = q.items.length
          ? `<select data-arg="${q.arg}">${q.items.map((i) => `<option value="${i.value}"${i.value === v ? ' selected' : ''}>${esc(i.name)}</option>`).join('')}</select>`
          : `<input type="range" data-arg="${q.arg}" min="${q.min}" max="${q.max}" step="${q.step}" value="${v}"><output>${v} ${UNITS[q.tag] || ''}</output>`;
        return `<label>${label}</label><div class="ctl">${control}</div>`;
      }).join('');
      const pre = p.pre.length
        ? `<label>Option <span class="test">test</span></label><div class="ctl"><select id="pre"><option value="">None</option>${p.pre.map((k) => `<option value="${k}"${k === this._pre ? ' selected' : ''}>${PRESELECT[k].label}</option>`).join('')}</select></div>`
        : '';
      body = `
        <div class="grid">
          <label>Product</label>
          <div class="ctl"><select id="product">${this._products.map((x, i) => `<option value="${i}"${x === p ? ' selected' : ''}>${esc(x.name)}</option>`).join('')}</select></div>
          ${rows}${pre}
        </div>
        <button id="brew">Brew</button>
        <div class="recipe" id="recipe" title="Copy the recipe">@TP:<span id="hex">${buildRecipe(p, this._values, this._changed, this._pre)}</span><ha-icon icon="mdi:content-copy"></ha-icon></div>
        <div class="msg">${esc(this._message)}</div>`;
    }
    root.innerHTML = `
      <style>
        ha-card { padding: 16px; }
        .head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
        .title { font-size: 1.3em; }
        .pill { display: inline-flex; align-items: center; gap: 6px; padding: 6px 12px; border-radius: 999px; font-size: 0.85rem;
                font-weight: 600; color: #fff; background: var(--st, var(--disabled-text-color, #9e9e9e)); }
        .pill ha-icon { --mdc-icon-size: 18px; }
        .pill.ready { --st: var(--success-color, #43a047); }
        .pill.blocked { --st: var(--warning-color, #ff9800); }
        .pill.off { --st: var(--error-color, #f44336); }
        .grid { display: grid; grid-template-columns: max-content 1fr; gap: 10px 12px; align-items: center; }
        .ctl { display: flex; align-items: center; gap: 8px; min-width: 0; }
        select, input { flex: 1; min-width: 0; font: inherit; color: var(--primary-text-color); }
        select { background: var(--card-background-color); border: 1px solid var(--divider-color); border-radius: 6px; padding: 6px; }
        input[type=range] { accent-color: var(--primary-color); }
        output { min-width: 4.5em; text-align: right; }
        .test { font-size: 0.75em; color: var(--warning-color, orange); border: 1px solid currentColor; border-radius: 4px; padding: 0 3px; }
        button { margin-top: 16px; width: 100%; padding: 10px; font: inherit; font-weight: 500; border: 0; border-radius: 8px;
                 background: var(--primary-color); color: var(--text-primary-color, #fff); cursor: pointer; }
        button:disabled { opacity: 0.4; cursor: default; }
        .recipe { margin-top: 8px; display: flex; align-items: center; gap: 6px; cursor: pointer; font-family: monospace; font-size: 0.8em; color: var(--secondary-text-color); }
        .recipe span { min-width: 0; overflow-wrap: anywhere; user-select: all; -webkit-user-select: all; }
        .recipe ha-icon { flex: none; --mdc-icon-size: 16px; }
        .msg { margin-top: 4px; color: var(--secondary-text-color); }
      </style>
      <ha-card>
        <div class="head"><span class="title">${esc(this._config.title)}</span><span id="status" class="pill"><ha-icon></ha-icon><span></span></span></div>
        ${body}
      </ha-card>`;
    this._bind();
    this._updateStatus();
  }

  _bind() {
    const root = this.shadowRoot;
    const product = root.getElementById('product');
    if (!product) return;
    product.addEventListener('change', (e) => this._select(Number(e.target.value)));
    root.getElementById('pre')?.addEventListener('change', (e) => { this._pre = e.target.value; this._message = ''; this._render(); });
    const hex = root.getElementById('hex');
    const msg = root.querySelector('.msg');
    const say = (t) => { this._message = t; msg.textContent = t; };
    root.querySelectorAll('[data-arg]').forEach((el) => {
      el.addEventListener('input', () => {
        const arg = el.dataset.arg;
        this._values[arg] = Number(el.value);
        this._changed.add(arg);
        const out = el.nextElementSibling;
        if (out && out.tagName === 'OUTPUT') {
          const q = this._product.params.find((x) => x.arg === arg);
          out.textContent = `${el.value} ${UNITS[q.tag] || ''}`;
        }
        hex.textContent = buildRecipe(this._product, this._values, this._changed, this._pre);
      });
    });
    root.getElementById('recipe').addEventListener('click', () => {
      copyText(hex.textContent).then(() => say('Recipe copied')).catch(() => say('Copy failed: select the code and copy it by hand'));
    });
    root.getElementById('brew').addEventListener('click', () => {
      const recipe = buildRecipe(this._product, this._values, this._changed, this._pre);
      const [domain, service] = this._config.action.split('.');
      this._hass.callService(domain, service, { recipe })
        .then(() => say(`Sent ${this._product.name}`))
        .catch((e) => say(`Failed: ${e.message || e}`));
    });
  }
}

customElements.define('jura-z10-card', JuraZ10Card);
window.customCards = window.customCards || [];
window.customCards.push({ type: 'jura-z10-card', name: 'Jura Z10', description: 'Brew a Jura Z10 product with its own parameters' });
