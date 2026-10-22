/*
 * The web platform for Soie's script extension, written over the
 * natives of script.c (the __soie object). Run first in every page.
 *
 * Wrappers for nodes are made by C with the prototype protoFor()
 * returns; their constructors are never called. Lists (childNodes,
 * querySelectorAll...) are plain arrays, made on each access: no live
 * collections.
 */
(function (api) {
'use strict';
const g = globalThis;
delete g.__soie;

/* Soie node types and namespaces, and the DOM's */
const S_DOC = 1, S_FRAG = 2, S_DOCTYPE = 3, S_EL = 4, S_TEXT = 5, S_COMMENT = 6;
const NODE_TYPE = [0, 9, 11, 10, 1, 3, 8];
const NS_URI = [null, 'http://www.w3.org/1999/xhtml',
	'http://www.w3.org/2000/svg', 'http://www.w3.org/1998/Math/MathML',
	'http://www.w3.org/1999/xlink',
	'http://www.w3.org/XML/1998/namespace', 'http://www.w3.org/2000/xmlns/'];
const NS_HTML = 1, NS_SVG = 2;

let document = null;	/* the document's wrapper, from setDocument() */
const LISTENERS = Symbol('listeners');
const HANDLERS = Symbol('handlers');
const STYLE = Symbol('style');

function report(e) {
	let s;
	try {
		s = e && e.stack ? e + '\n' + e.stack : String(e);
	} catch (x) {
		s = 'error';
	}
	api.message(2, s);
}

function str(v) {
	return v === undefined ? 'undefined' : v === null ? 'null' : String(v);
}

/* DOMException */
const EXC = { IndexSizeError: 1, HierarchyRequestError: 3,
	WrongDocumentError: 4, InvalidCharacterError: 5,
	NoModificationAllowedError: 7, NotFoundError: 8,
	NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
	InvalidAccessError: 15, SecurityError: 18, NetworkError: 19,
	AbortError: 20, QuotaExceededError: 22, TimeoutError: 23,
	DataCloneError: 25 };
class DOMException extends Error {
	constructor(message = '', name = 'Error') {
		super(message);
		this.name = name;
	}
	get code() { return EXC[this.name] || 0; }
}
for (const k in EXC)
	DOMException[k.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase()
		.replace(/_ERROR$/, '_ERR')] = EXC[k];

function list(a) {
	a.item = (i) => a[i] === undefined ? null : a[i];
	return a;
}

/* Events */

class Event {
	constructor(type, init = {}) {
		if (arguments.length < 1)
			throw new TypeError('Event: type required');
		this.type = String(type);
		this.bubbles = !!init.bubbles;
		this.cancelable = !!init.cancelable;
		this.composed = !!init.composed;
		this.defaultPrevented = false;
		this.isTrusted = false;
		this.target = null;
		this.currentTarget = null;
		this.eventPhase = 0;
		this.timeStamp = api.now() - start;
		this._stop = false;
		this._stopNow = false;
		this._passive = false;
	}
	get srcElement() { return this.target; }
	get returnValue() { return !this.defaultPrevented; }
	set returnValue(v) { if (!v) this.preventDefault(); }
	get cancelBubble() { return this._stop; }
	set cancelBubble(v) { if (v) this._stop = true; }
	preventDefault() {
		if (this.cancelable && !this._passive)
			this.defaultPrevented = true;
	}
	stopPropagation() { this._stop = true; }
	stopImmediatePropagation() { this._stop = this._stopNow = true; }
	composedPath() { return this._path ? this._path.slice() : []; }
	initEvent(type, bubbles, cancelable) {
		this.type = String(type);
		this.bubbles = !!bubbles;
		this.cancelable = !!cancelable;
	}
}
Event.NONE = 0;
Event.CAPTURING_PHASE = 1;
Event.AT_TARGET = 2;
Event.BUBBLING_PHASE = 3;

class CustomEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.detail = init.detail === undefined ? null : init.detail;
	}
	initCustomEvent(type, b, c, detail) {
		this.initEvent(type, b, c);
		this.detail = detail;
	}
}
class UIEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.view = init.view || null;
		this.detail = init.detail || 0;
	}
}
class MouseEvent extends UIEvent {
	constructor(type, init = {}) {
		super(type, init);
		for (const k of ['clientX', 'clientY', 'screenX', 'screenY',
			'button', 'buttons'])
			this[k] = +init[k] || 0;
		for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey'])
			this[k] = !!init[k];
		this.relatedTarget = init.relatedTarget || null;
	}
	get pageX() { return this.clientX + g.scrollX; }
	get pageY() { return this.clientY + g.scrollY; }
	get x() { return this.clientX; }
	get y() { return this.clientY; }
	get offsetX() { return this.clientX; }
	get offsetY() { return this.clientY; }
	get which() { return this.button + 1; }
	getModifierState(k) {
		return { Control: this.ctrlKey, Shift: this.shiftKey,
			Alt: this.altKey, Meta: this.metaKey }[k] || false;
	}
}
class PointerEvent extends MouseEvent {
	constructor(type, init = {}) {
		super(type, init);
		this.pointerId = init.pointerId || 1;
		this.pointerType = init.pointerType || 'mouse';
		this.isPrimary = true;
	}
}
class WheelEvent extends MouseEvent {
	constructor(type, init = {}) {
		super(type, init);
		this.deltaX = init.deltaX || 0;
		this.deltaY = init.deltaY || 0;
		this.deltaZ = 0;
		this.deltaMode = 0;
	}
}
const KEY_CODES = { Enter: 13, Tab: 9, Escape: 27, Backspace: 8,
	Delete: 46, ' ': 32, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39,
	ArrowDown: 40, Home: 36, End: 35, PageUp: 33, PageDown: 34,
	Shift: 16, Control: 17, Alt: 18 };
class KeyboardEvent extends UIEvent {
	constructor(type, init = {}) {
		super(type, init);
		this.key = init.key || '';
		this.code = init.code || (this.key.length === 1 ?
			(/[a-z]/i.test(this.key) ? 'Key' + this.key.toUpperCase() :
			 /[0-9]/.test(this.key) ? 'Digit' + this.key : '') :
			this.key);
		for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey',
			'repeat', 'isComposing'])
			this[k] = !!init[k];
		this.location = 0;
	}
	get keyCode() {
		if (KEY_CODES[this.key])
			return KEY_CODES[this.key];
		return this.key.length === 1 ?
			this.key.toUpperCase().charCodeAt(0) : 0;
	}
	get which() { return this.keyCode; }
	get charCode() {
		return this.type === 'keypress' && this.key.length === 1 ?
			this.key.charCodeAt(0) : 0;
	}
	getModifierState(k) {
		return { Control: this.ctrlKey, Shift: this.shiftKey,
			Alt: this.altKey, Meta: this.metaKey }[k] || false;
	}
}
class FocusEvent extends UIEvent {
	constructor(type, init = {}) {
		super(type, init);
		this.relatedTarget = init.relatedTarget || null;
	}
}
class InputEvent extends UIEvent {
	constructor(type, init = {}) {
		super(type, init);
		this.data = init.data === undefined ? null : init.data;
		this.inputType = init.inputType || '';
		this.isComposing = false;
	}
}
class SubmitEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.submitter = init.submitter || null;
	}
}
class ProgressEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.lengthComputable = !!init.lengthComputable;
		this.loaded = init.loaded || 0;
		this.total = init.total || 0;
	}
}
class ErrorEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.message = init.message || '';
		this.error = init.error || null;
		this.filename = init.filename || '';
		this.lineno = init.lineno || 0;
		this.colno = 0;
	}
}
class PopStateEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.state = init.state === undefined ? null : init.state;
	}
}
class HashChangeEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.oldURL = init.oldURL || '';
		this.newURL = init.newURL || '';
	}
}
class MessageEvent extends Event {
	constructor(type, init = {}) {
		super(type, init);
		this.data = init.data === undefined ? null : init.data;
		this.origin = init.origin || '';
		this.source = init.source || null;
		this.ports = [];
	}
}

class EventTarget {
	addEventListener(type, fn, opts) {
		if (!fn)
			return;
		const capture = typeof opts === 'boolean' ? opts :
			!!(opts && opts.capture);
		const once = !!(opts && typeof opts === 'object' && opts.once);
		const passive = !!(opts && typeof opts === 'object' &&
			opts.passive);
		const m = this[LISTENERS] || (this[LISTENERS] = Object.create(null));
		const l = m[type] || (m[type] = []);
		if (l.some((x) => x.fn === fn && x.capture === capture))
			return;
		const entry = { fn, capture, once, passive };
		l.push(entry);
		if (opts && typeof opts === 'object' && opts.signal)
			opts.signal.addEventListener('abort',
				() => this.removeEventListener(type, fn, opts));
	}
	removeEventListener(type, fn, opts) {
		const capture = typeof opts === 'boolean' ? opts :
			!!(opts && opts.capture);
		const m = this[LISTENERS];
		if (!m || !m[type])
			return;
		const i = m[type].findIndex((x) => x.fn === fn &&
			x.capture === capture);
		if (i >= 0) {
			m[type][i].removed = true;
			m[type].splice(i, 1);
		}
	}
	dispatchEvent(ev) {
		if (!(ev instanceof Event))
			throw new TypeError('dispatchEvent: not an Event');
		return dispatch(this, ev);
	}
}

/* Window events whose handlers are <body> attributes */
const BODY_WINDOW = /^(load|unload|beforeunload|resize|scroll|error|message|popstate|hashchange|focus|blur|online|offline|pageshow|pagehide|storage)$/;

/* The handler of an on* attribute or property, as a function. */
function handlerOf(target, type) {
	const h = target[HANDLERS] && target[HANDLERS][type];
	if (h !== undefined)
		return h;
	let el = null;
	if (target === g)
		el = BODY_WINDOW.test(type) ? document.body : null;
	else if (target instanceof Element)
		el = target;
	const src = el && el.getAttribute && el.getAttribute('on' + type);
	if (!src)
		return null;
	const cache = el._onsrc || (el._onsrc = Object.create(null));
	if (cache[type] && cache[type].src === src)
		return cache[type].fn;
	let fn = null;
	try {
		fn = new Function('event', src);
	} catch (e) {
		report(e);
	}
	cache[type] = { src, fn };
	return fn;
}

function invoke(node, ev, phase) {
	ev.currentTarget = node;
	ev.eventPhase = phase;
	if (phase !== Event.CAPTURING_PHASE) {
		const h = handlerOf(node, ev.type);
		if (typeof h === 'function') {
			try {
				const r = h.call(node, ev);
				if (r === false)
					ev.preventDefault();
			} catch (e) {
				report(e);
			}
		}
	}
	const m = node[LISTENERS];
	if (!m || !m[ev.type])
		return;
	for (const l of m[ev.type].slice()) {
		if (l.removed)
			continue;
		if ((phase === Event.CAPTURING_PHASE && !l.capture) ||
		    (phase === Event.BUBBLING_PHASE && l.capture))
			continue;
		if (l.once)
			node.removeEventListener(ev.type, l.fn, l.capture);
		ev._passive = l.passive;
		try {
			if (typeof l.fn === 'function')
				l.fn.call(node, ev);
			else if (l.fn && typeof l.fn.handleEvent === 'function')
				l.fn.handleEvent(ev);
		} catch (e) {
			report(e);
		}
		ev._passive = false;
		if (ev._stopNow)
			break;
	}
}

function dispatch(target, ev) {
	const path = [target];
	if (target instanceof Node) {
		for (let n = api.parent(target); n; n = api.parent(n))
			path.push(n);
		if (path[path.length - 1] === document && ev.type !== 'load')
			path.push(g);
	}
	ev.target = target;
	ev._path = path;
	ev._stop = ev._stopNow = false;
	for (let i = path.length - 1; i > 0 && !ev._stop; i--)
		invoke(path[i], ev, Event.CAPTURING_PHASE);
	if (!ev._stop)
		invoke(target, ev, Event.AT_TARGET);
	if (ev.bubbles)
		for (let i = 1; i < path.length && !ev._stop; i++)
			invoke(path[i], ev, Event.BUBBLING_PHASE);
	ev.currentTarget = null;
	ev.eventPhase = 0;
	return !ev.defaultPrevented;
}

/* Events from the host: dispatch(target or null, type, init) */
const MOUSE = /^(click|dblclick|mouse(down|up|move|over|out|enter|leave)|contextmenu|auxclick)$/;
const POINTER = /^pointer/;
const NO_BUBBLE = /^(focus|blur|load|unload|error|abort|scroll|resize|mouseenter|mouseleave|pointerenter|pointerleave|loadstart|loadend|progress|play|pause|playing|ended|timeupdate|durationchange|volumechange|loadeddata|loadedmetadata|canplay|canplaythrough|seeking|seeked|waiting|emptied|ratechange|invalid|toggle)$/;
const NOT_CANCELABLE = /^(input|change|focus|blur|focusin|focusout|load|unload|error|scroll|resize|mouseenter|mouseleave|DOMContentLoaded|readystatechange|keyup|play|pause|ended|timeupdate)$/;
api.dispatch = function (target, type, init) {
	const t = target || g;
	const opts = { bubbles: !NO_BUBBLE.test(type) || (type === 'scroll' &&
		t === document), cancelable: !NOT_CANCELABLE.test(type),
		clientX: init.clientX, clientY: init.clientY,
		button: init.button, buttons: type === 'mousedown' ? 1 : 0,
		shiftKey: !!(init.mods & 1), ctrlKey: !!(init.mods & 2),
		altKey: !!(init.mods & 4), metaKey: !!(init.mods & 8),
		key: init.key, detail: 1, view: g };
	let ev;
	if (type === 'readystatechange' && t === document) {
		readyState = 'interactive';
	} else if (type === 'load' && !target) {
		readyState = 'complete';
		dispatch(document, new Event('readystatechange'));
	}
	if (MOUSE.test(type))
		ev = new MouseEvent(type, opts);
	else if (POINTER.test(type))
		ev = new PointerEvent(type, opts);
	else if (type === 'wheel')
		ev = new WheelEvent(type, opts);
	else if (/^key/.test(type))
		ev = new KeyboardEvent(type, opts);
	else if (/^focus|^blur/.test(type))
		ev = new FocusEvent(type, opts);
	else if (type === 'input')
		ev = new InputEvent(type, Object.assign(opts,
			{ inputType: 'insertText' }));
	else if (type === 'submit')
		ev = new SubmitEvent(type, opts);
	else
		ev = new Event(type, opts);
	ev.isTrusted = true;
	if (type === 'focus' && t instanceof Element)
		activeElement = t;
	if (type === 'blur' && t === activeElement)
		activeElement = null;
	const ok = dispatch(t, ev);
	if (ok && type === 'click' && t instanceof Element)
		activation(t, true);
	return !ok;
};

/* Default actions the host does not know: scripts' own, after a
 * click on these elements. host: the host does the navigation. */
function activation(el, host) {
	for (let n = el; n instanceof Element; n = n.parentNode) {
		const tag = n.localName;
		if (tag === 'label' && n.control && n.control !== el) {
			n.control.click();
			return;
		}
		if (n.namespaceURI !== NS_URI[NS_HTML])
			continue;
		if (tag === 'a' && n.hasAttribute('href')) {
			if (!host) {
				const h = n.getAttribute('href');
				if (!/^\s*javascript:/i.test(h))
					api.navigate(n.href, false);
				else
					try {
						(0, eval)(decodeURIComponent(
							h.replace(/^\s*javascript:/i, '')));
					} catch (e) {
						report(e);
					}
			}
			return;
		}
		if ((tag === 'button' && (n.type === 'submit')) ||
		    (tag === 'input' && (n.type === 'submit' ||
		     n.type === 'image'))) {
			if (!host && n.form)
				n.form.requestSubmit(n);
			return;
		}
	}
}

/* Script elements inserted by scripts: inline ones ran at once, the
 * external ones run from a task, as async scripts do */
api.later = function (el) {
	addTimer(() => api.runScript(el), 0, [], false);
};

/* Timers */
const start = api.now();
const timers = new Map();
let timerId = 0;

function addTimer(fn, delay, args, repeat) {
	const id = ++timerId;
	delay = Math.max(0, +delay || 0);
	timers.set(id, { fn, args, due: api.now() + delay,
		every: repeat ? Math.max(delay, 4) : -1 });
	return id;
}

api.tick = function () {
	const now = api.now();
	const due = [];
	for (const [id, t] of timers)
		if (t.due <= now)
			due.push([id, t]);
	due.sort((a, b) => a[1].due - b[1].due || a[0] - b[0]);
	for (const [id, t] of due) {
		if (timers.get(id) !== t)
			continue;	/* cleared by an earlier one */
		if (t.every >= 0)
			t.due = now + t.every;
		else
			timers.delete(id);
		try {
			if (typeof t.fn === 'function')
				t.fn.apply(g, t.args);
			else
				(0, eval)(String(t.fn));
		} catch (e) {
			report(e);
		}
	}
	let next = -1;
	const later = api.now();
	for (const t of timers.values()) {
		const d = Math.max(0, t.due - later);
		if (next < 0 || d < next)
			next = d;
	}
	return next;
};

/* Nodes */

function check(n) {
	if (!(n instanceof Node))
		throw new TypeError('not a Node');
	return n;
}

function toNodes(args) {
	if (args.length === 1 && args[0] instanceof Node)
		return args[0];
	const f = api.createFragment();
	for (const a of args)
		api.insert(f, a instanceof Node ? a : api.createText(str(a)), null);
	return f;
}

function insert(parent, node, ref) {
	check(node);
	if (ref !== null && ref !== undefined && (!(ref instanceof Node) ||
	    api.parent(ref) !== parent))
		throw new DOMException('reference is not a child', 'NotFoundError');
	const moved = customs.size ? (api.type(node) === S_FRAG ?
		api.children(node, true) : [node]) : null;
	if (!api.insert(parent, node, ref || null))
		throw new DOMException('cannot insert there',
			'HierarchyRequestError');
	if (moved)
		connected(moved);
	return node;
}

/* connectedCallback of custom elements just inserted */
function connected(nodes) {
	for (const n of nodes) {
		if (api.type(n) !== S_EL || !n.isConnected)
			continue;
		for (const el of [n, ...query(n, '*', 0)])
			if (customs.has(api.name(el)) &&
			    typeof el.connectedCallback === 'function')
				try {
					el.connectedCallback();
				} catch (e) {
					report(e);
				}
	}
}

function textOf(n) {
	const t = api.type(n);
	if (t === S_TEXT || t === S_COMMENT)
		return api.data(n);
	if (t === S_EL || t === S_FRAG)
		return api.text(n);
	return null;
}

class Node extends EventTarget {
	constructor() {
		super();
		throw new TypeError('Illegal constructor');
	}
	get nodeType() { return NODE_TYPE[api.type(this)]; }
	get nodeName() {
		switch (api.type(this)) {
		case S_EL: return this.tagName;
		case S_TEXT: return '#text';
		case S_COMMENT: return '#comment';
		case S_DOC: return '#document';
		case S_FRAG: return '#document-fragment';
		default: return api.name(this);
		}
	}
	get nodeValue() {
		const t = api.type(this);
		return t === S_TEXT || t === S_COMMENT ? api.data(this) : null;
	}
	set nodeValue(v) {
		const t = api.type(this);
		if (t === S_TEXT || t === S_COMMENT)
			api.setData(this, v === null ? '' : str(v));
	}
	get textContent() { return textOf(this); }
	set textContent(v) {
		const t = api.type(this);
		if (t === S_TEXT || t === S_COMMENT) {
			api.setData(this, v === null ? '' : str(v));
			return;
		}
		if (t !== S_EL && t !== S_FRAG)
			return;
		for (let k; (k = api.first(this)); )
			api.remove(k);
		v = v === null || v === undefined ? '' : String(v);
		if (v !== '')
			api.insert(this, api.createText(v), null);
	}
	get ownerDocument() {
		return api.type(this) === S_DOC ? null : document;
	}
	get parentNode() { return api.parent(this); }
	get parentElement() {
		const p = api.parent(this);
		return p && api.type(p) === S_EL ? p : null;
	}
	get childNodes() { return list(api.children(this, false)); }
	get firstChild() { return api.first(this); }
	get lastChild() { return api.last(this); }
	get nextSibling() { return api.next(this); }
	get previousSibling() { return api.prev(this); }
	get isConnected() {
		let n = this;
		for (let p; (p = api.parent(n)); )
			n = p;
		return n === document;
	}
	get baseURI() { return document.baseURI; }
	hasChildNodes() { return !!api.first(this); }
	getRootNode() {
		let n = this;
		for (let p; (p = api.parent(n)); )
			n = p;
		return n;
	}
	appendChild(n) { return insert(this, n, null); }
	insertBefore(n, ref) { return insert(this, n, ref); }
	removeChild(n) {
		check(n);
		if (api.parent(n) !== this)
			throw new DOMException('not a child', 'NotFoundError');
		api.remove(n);
		return n;
	}
	replaceChild(n, old) {
		check(old);
		if (api.parent(old) !== this)
			throw new DOMException('not a child', 'NotFoundError');
		if (n === old)
			return old;
		const ref = api.next(old) === n ? api.next(n) : api.next(old);
		api.remove(old);
		insert(this, n, ref);
		return old;
	}
	cloneNode(deep) {
		const c = api.clone(this, !!deep);
		return c;
	}
	contains(n) {
		for (; n; n = api.parent(n))
			if (n === this)
				return true;
		return false;
	}
	isSameNode(n) { return n === this; }
	isEqualNode(n) {
		if (!(n instanceof Node) || n.nodeType !== this.nodeType)
			return false;
		const a = api.type(this);
		if (a === S_EL)
			return api.serialize(this, true) === api.serialize(n, true);
		if (a === S_TEXT || a === S_COMMENT)
			return api.data(this) === api.data(n);
		return api.serialize(this, false) === api.serialize(n, false);
	}
	compareDocumentPosition(other) {
		if (other === this)
			return 0;
		const chain = (n) => {
			const c = [];
			for (; n; n = api.parent(n))
				c.unshift(n);
			return c;
		};
		const a = chain(this), b = chain(other);
		if (a[0] !== b[0])
			return 1 | 32 | 2;
		let i = 0;
		while (i < a.length && i < b.length && a[i] === b[i])
			i++;
		if (i === a.length)
			return 16 | 4;	/* other is inside this */
		if (i === b.length)
			return 8 | 2;	/* this is inside other */
		for (let k = api.next(a[i]); k; k = api.next(k))
			if (k === b[i])
				return 4;
		return 2;
	}
	normalize() {
		for (let k = api.first(this); k; ) {
			const next = api.next(k);
			if (api.type(k) === S_TEXT) {
				if (api.data(k) === '') {
					api.remove(k);
				} else if (next && api.type(next) === S_TEXT) {
					api.setData(k, api.data(k) + api.data(next));
					api.remove(next);
					continue;
				}
			} else if (api.type(k) === S_EL) {
				k.normalize();
			}
			k = next;
		}
	}
	lookupNamespaceURI() { return null; }
	isDefaultNamespace(ns) { return ns === NS_URI[NS_HTML]; }
}
for (const [k, v] of Object.entries({ ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2,
	TEXT_NODE: 3, CDATA_SECTION_NODE: 4, PROCESSING_INSTRUCTION_NODE: 7,
	COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10,
	DOCUMENT_FRAGMENT_NODE: 11, DOCUMENT_POSITION_DISCONNECTED: 1,
	DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4,
	DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 })) {
	Node[k] = v;
	Node.prototype[k] = v;
}

/* ChildNode and ParentNode mixins */
const childNode = {
	remove() {
		if (api.parent(this))
			api.remove(this);
	},
	before(...a) {
		const p = api.parent(this);
		if (p)
			insert(p, toNodes(a), this);
	},
	after(...a) {
		const p = api.parent(this);
		if (p)
			insert(p, toNodes(a), api.next(this));
	},
	replaceWith(...a) {
		const p = api.parent(this);
		if (!p)
			return;
		const ref = api.next(this);
		const n = toNodes(a);
		api.remove(this);
		insert(p, n, ref && api.parent(ref) === p ? ref : null);
	},
	get nextElementSibling() {
		let k = api.next(this);
		while (k && api.type(k) !== S_EL)
			k = api.next(k);
		return k;
	},
	get previousElementSibling() {
		let k = api.prev(this);
		while (k && api.type(k) !== S_EL)
			k = api.prev(k);
		return k;
	},
};

function escapeIdent(s) {
	return String(s).replace(/[^a-zA-Z0-9_\u00a0-\uffff-]/g,
		(c) => '\\' + c.charCodeAt(0).toString(16) + ' ')
		.replace(/^(-?)([0-9])/, (m, a, b) => a + '\\3' + b + ' ')
		.replace(/^-$/, '\\-');
}

function query(root, sel, max) {
	try {
		return api.query(root, String(sel), max);
	} catch (e) {
		throw new DOMException("'" + sel + "' is not a valid selector",
			'SyntaxError');
	}
}

const parentNode = {
	get children() {
		const a = list(api.children(this, true));
		a.namedItem = (name) => a.find((e) => e.id === name ||
			e.getAttribute('name') === name) || null;
		return a;
	},
	get firstElementChild() {
		let k = api.first(this);
		while (k && api.type(k) !== S_EL)
			k = api.next(k);
		return k;
	},
	get lastElementChild() {
		let k = api.last(this);
		while (k && api.type(k) !== S_EL)
			k = api.prev(k);
		return k;
	},
	get childElementCount() { return api.children(this, true).length; },
	append(...a) { insert(this, toNodes(a), null); },
	prepend(...a) { insert(this, toNodes(a), api.first(this)); },
	replaceChildren(...a) {
		const n = toNodes(a);
		for (let k; (k = api.first(this)); )
			api.remove(k);
		insert(this, n, null);
	},
	querySelector(sel) { return query(this, sel, 1)[0] || null; },
	querySelectorAll(sel) { return list(query(this, sel, 0)); },
	getElementsByTagName(name) {
		name = String(name);
		return list(query(this, name === '*' ? '*' :
			escapeIdent(name), 0));
	},
	getElementsByTagNameNS(ns, name) {
		return this.getElementsByTagName(name);
	},
	getElementsByClassName(names) {
		const c = String(names).trim().split(/\s+/).filter(Boolean);
		if (!c.length)
			return list([]);
		return list(query(this, c.map((x) => '.' + escapeIdent(x))
			.join(''), 0));
	},
};

function mixin(cls, m) {
	for (const k of Object.getOwnPropertyNames(m))
		Object.defineProperty(cls.prototype, k,
			Object.getOwnPropertyDescriptor(m, k));
}

class CharacterData extends Node {
	get data() { return api.data(this); }
	set data(v) { api.setData(this, v === null ? '' : str(v)); }
	get length() { return api.data(this).length; }
	appendData(s) { this.data += s; }
	insertData(o, s) {
		const d = this.data;
		this.data = d.slice(0, o) + s + d.slice(o);
	}
	deleteData(o, n) {
		const d = this.data;
		this.data = d.slice(0, o) + d.slice(o + n);
	}
	replaceData(o, n, s) {
		const d = this.data;
		this.data = d.slice(0, o) + s + d.slice(o + n);
	}
	substringData(o, n) { return this.data.substr(o, n); }
}
mixin(CharacterData, childNode);

class Text extends CharacterData {
	constructor(data = '') {
		return api.createText(str(data));
	}
	get wholeText() { return this.data; }
	splitText(o) {
		const d = this.data;
		const t = api.createText(d.slice(o));
		this.data = d.slice(0, o);
		const p = api.parent(this);
		if (p)
			api.insert(p, t, api.next(this));
		return t;
	}
}
class CDATASection extends Text {}
class Comment extends CharacterData {
	constructor(data = '') {
		return api.createComment(str(data));
	}
}
class DocumentType extends Node {
	get name() { return api.name(this); }
	get publicId() { return ''; }
	get systemId() { return ''; }
}
mixin(DocumentType, { remove: childNode.remove });

class DocumentFragment extends Node {
	constructor() {
		return api.createFragment();
	}
	getElementById(id) {
		return query(this, '#' + escapeIdent(id), 1)[0] || null;
	}
}
mixin(DocumentFragment, parentNode);

/* Attributes */

class Attr {
	constructor(el, name, value, ns) {
		this.ownerElement = el;
		this.name = name;
		this.localName = name.replace(/^[^:]*:/, '');
		this.namespaceURI = NS_URI[ns] || null;
		this.prefix = name.includes(':') ? name.split(':')[0] : null;
		this._value = value;
	}
	get value() { return this._value; }
	set value(v) {
		this._value = String(v);
		if (this.ownerElement)
			this.ownerElement.setAttribute(this.name, this._value);
	}
	get nodeValue() { return this._value; }
	get nodeName() { return this.name; }
	get specified() { return true; }
}

class DOMTokenList {
	constructor(el, attr) {
		this._el = el;
		this._attr = attr;
	}
	_get() {
		const v = this._el.getAttribute(this._attr);
		return v ? v.trim().split(/[ \t\n\f\r]+/).filter(Boolean) : [];
	}
	_set(a) { this._el.setAttribute(this._attr, a.join(' ')); }
	get length() { return [...new Set(this._get())].length; }
	get value() { return this._el.getAttribute(this._attr) || ''; }
	set value(v) { this._el.setAttribute(this._attr, String(v)); }
	item(i) {
		const a = [...new Set(this._get())];
		return i < a.length ? a[i] : null;
	}
	contains(t) { return this._get().includes(String(t)); }
	add(...t) {
		const a = this._get();
		for (const x of t) {
			if (x === '' || /\s/.test(x))
				throw new DOMException('bad token', x === '' ?
					'SyntaxError' : 'InvalidCharacterError');
			if (!a.includes(String(x)))
				a.push(String(x));
		}
		this._set([...new Set(a)]);
	}
	remove(...t) {
		const r = t.map(String);
		this._set([...new Set(this._get().filter((x) => !r.includes(x)))]);
	}
	toggle(t, force) {
		t = String(t);
		const has = this.contains(t);
		if (force === undefined ? has : !force) {
			if (has)
				this.remove(t);
			return false;
		}
		if (!has)
			this.add(t);
		return true;
	}
	replace(a, b) {
		const l = this._get();
		const i = l.indexOf(String(a));
		if (i < 0)
			return false;
		l[i] = String(b);
		this._set([...new Set(l)]);
		return true;
	}
	supports() { return true; }
	forEach(fn, self) { [...new Set(this._get())].forEach(fn, self); }
	entries() { return [...new Set(this._get())].entries(); }
	keys() { return [...new Set(this._get())].keys(); }
	values() { return [...new Set(this._get())].values(); }
	[Symbol.iterator]() { return this.values(); }
	toString() { return this.value; }
}

/* Inline style: the style attribute as a declaration list */

function camelToKebab(p) {
	if (p === 'cssFloat')
		return 'float';
	if (p.startsWith('--'))
		return p;
	return p.replace(/^(webkit|moz|ms|o)(?=[A-Z])/, '-$1')
		.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
}

function parseDecls(s) {
	const out = [];
	let i = 0, depth = 0, quote = null, startDecl = 0;
	s = s || '';
	const push = (d) => {
		const c = d.indexOf(':');
		if (c < 0)
			return;
		const name = d.slice(0, c).trim();
		let value = d.slice(c + 1).trim(), prio = '';
		const m = /\s*!\s*important\s*$/i.exec(value);
		if (m) {
			value = value.slice(0, m.index);
			prio = 'important';
		}
		if (name)
			out.push([name.startsWith('--') ? name :
				name.toLowerCase(), value, prio]);
	};
	for (; i < s.length; i++) {
		const c = s[i];
		if (quote) {
			if (c === '\\')
				i++;
			else if (c === quote)
				quote = null;
		} else if (c === '"' || c === "'") {
			quote = c;
		} else if (c === '(') {
			depth++;
		} else if (c === ')') {
			depth--;
		} else if (c === ';' && depth <= 0) {
			push(s.slice(startDecl, i));
			startDecl = i + 1;
		}
	}
	push(s.slice(startDecl));
	return out;
}

class CSSStyleDeclaration {
	constructor(el) { this._el = el; }
	_get() { return parseDecls(this._el.getAttribute('style')); }
	_set(d) {
		const s = d.map(([n, v, p]) => n + ': ' + v +
			(p ? ' !important' : '') + ';').join(' ');
		if (s || this._el.hasAttribute('style'))
			this._el.setAttribute('style', s);
	}
	get cssText() { return this._el.getAttribute('style') || ''; }
	set cssText(v) { this._el.setAttribute('style', str(v)); }
	get length() { return this._get().length; }
	item(i) {
		const d = this._get();
		return i < d.length ? d[i][0] : '';
	}
	getPropertyValue(name) {
		name = String(name);
		if (!name.startsWith('--'))
			name = name.toLowerCase();
		const d = this._get().filter((x) => x[0] === name);
		return d.length ? d[d.length - 1][1] : '';
	}
	getPropertyPriority(name) {
		const d = this._get().filter((x) => x[0] === name);
		return d.length ? d[d.length - 1][2] : '';
	}
	setProperty(name, value, prio) {
		name = String(name);
		if (!name.startsWith('--'))
			name = name.toLowerCase();
		value = value === null || value === undefined ? '' :
			String(value).trim();
		const d = this._get().filter((x) => x[0] !== name);
		if (value !== '')
			d.push([name, value, prio === 'important' ?
				'important' : '']);
		this._set(d);
	}
	removeProperty(name) {
		const v = this.getPropertyValue(name);
		this._set(this._get().filter((x) => x[0] !== name));
		return v;
	}
}

const styleHandler = {
	get(t, p, r) {
		if (typeof p !== 'string' || p in t)
			return Reflect.get(t, p, r);
		if (/^\d+$/.test(p))
			return t.item(+p);
		return t.getPropertyValue(camelToKebab(p));
	},
	set(t, p, v, r) {
		if (typeof p !== 'string' || p in t || p.startsWith('_'))
			return Reflect.set(t, p, v, r);
		t.setProperty(camelToKebab(p), v);
		return true;
	},
	has(t, p) { return true; },
};

/* Computed style: values from the cascade, read on demand */
function computedStyle(el, pseudo) {
	const t = {
		getPropertyValue: (p) => api.style(el, String(p), pseudo || null),
		getPropertyPriority: () => '',
		setProperty() {
			throw new DOMException('computed style',
				'NoModificationAllowedError');
		},
		removeProperty() {
			throw new DOMException('computed style',
				'NoModificationAllowedError');
		},
		item: () => '',
		length: 0,
	};
	return new Proxy(t, {
		get(o, p) {
			if (typeof p !== 'string' || p in o)
				return o[p];
			if (p === 'cssText')
				return '';
			return api.style(el, camelToKebab(p), pseudo || null);
		},
		set() { return true; },
	});
}

/* Elements */

function lowerHTML(el, name) {
	name = String(name);
	return api.ns(el) === NS_HTML ? name.toLowerCase() : name;
}

function reflect(cls, attrs, kind) {
	for (const a of attrs) {
		const [prop, attr] = Array.isArray(a) ? a : [a, a.toLowerCase()];
		if (Object.prototype.hasOwnProperty.call(cls.prototype, prop))
			continue;
		const d = kind === 'bool' ? {
			get() { return api.getAttr(this, attr) !== null; },
			set(v) {
				if (v)
					api.setAttr(this, attr, '');
				else
					api.removeAttr(this, attr);
			},
		} : kind === 'url' ? {
			get() {
				const v = api.getAttr(this, attr);
				return v === null ? '' :
					api.resolve(document.baseURI, v) || v;
			},
			set(v) { api.setAttr(this, attr, str(v)); },
		} : kind === 'int' ? {
			get() {
				const v = parseInt(api.getAttr(this, attr), 10);
				return isNaN(v) ? 0 : v;
			},
			set(v) { api.setAttr(this, attr, String(v | 0)); },
		} : {
			get() {
				const v = api.getAttr(this, attr);
				return v === null ? '' : v;
			},
			set(v) { api.setAttr(this, attr, str(v)); },
		};
		d.configurable = true;
		Object.defineProperty(cls.prototype, prop, d);
	}
}

class Element extends Node {
	get localName() { return api.name(this); }
	get tagName() {
		const n = api.name(this);
		return api.ns(this) === NS_HTML ? n.toUpperCase() : n;
	}
	get namespaceURI() { return NS_URI[api.ns(this)]; }
	get prefix() { return null; }
	get id() { return api.getAttr(this, 'id') || ''; }
	set id(v) { api.setAttr(this, 'id', str(v)); }
	get className() { return api.getAttr(this, 'class') || ''; }
	set className(v) { api.setAttr(this, 'class', str(v)); }
	get classList() {
		return this._classList ||
			(this._classList = new DOMTokenList(this, 'class'));
	}
	set classList(v) { this.className = v; }
	get attributes() {
		const a = api.attrs(this).map(([n, v, ns]) =>
			new Attr(this, n, v, ns));
		a.getNamedItem = (n) => a.find((x) => x.name === n) || null;
		a.item = (i) => a[i] || null;
		for (const x of a)
			if (!(x.name in a))
				a[x.name] = x;
		return a;
	}
	hasAttributes() { return api.attrs(this).length > 0; }
	getAttributeNames() { return api.attrs(this).map((x) => x[0]); }
	getAttribute(n) { return api.getAttr(this, lowerHTML(this, n)); }
	getAttributeNS(ns, n) { return api.getAttr(this, String(n)); }
	setAttribute(n, v) {
		n = lowerHTML(this, n);
		if (!/^[^\s"'>\/=\0]+$/.test(n))
			throw new DOMException('bad attribute name',
				'InvalidCharacterError');
		api.setAttr(this, n, str(v));
		if (n === 'src' && this._loadOnSrc)
			this._loadOnSrc();
	}
	setAttributeNS(ns, n, v) {
		this.setAttribute(String(n).replace(/^xlink:|^xml:/, ''), v);
	}
	removeAttribute(n) { api.removeAttr(this, lowerHTML(this, n)); }
	removeAttributeNS(ns, n) { api.removeAttr(this, String(n)); }
	hasAttribute(n) { return api.getAttr(this, lowerHTML(this, n)) !== null; }
	hasAttributeNS(ns, n) { return api.getAttr(this, String(n)) !== null; }
	toggleAttribute(n, force) {
		const has = this.hasAttribute(n);
		if (force === undefined ? has : !force) {
			this.removeAttribute(n);
			return false;
		}
		if (!has)
			this.setAttribute(n, '');
		return true;
	}
	getAttributeNode(n) {
		const v = this.getAttribute(n);
		return v === null ? null : new Attr(this, lowerHTML(this, n), v, 0);
	}
	setAttributeNode(a) { this.setAttribute(a.name, a.value); }
	get innerHTML() {
		return api.serialize(this, false);
	}
	set innerHTML(v) {
		const target = api.content(this) || this;
		for (let k; (k = api.first(target)); )
			api.remove(k);
		v = v === null ? '' : String(v);
		if (v !== '')
			api.insert(target, api.parse(this, v), null);
	}
	get outerHTML() { return api.serialize(this, true); }
	set outerHTML(v) {
		const p = api.parent(this);
		if (!p)
			return;
		if (api.type(p) === S_DOC)
			throw new DOMException('outerHTML of the root',
				'NoModificationAllowedError');
		const f = api.parse(api.type(p) === S_EL ? p : null, str(v));
		api.insert(p, f, this);
		api.remove(this);
	}
	insertAdjacentHTML(pos, html) {
		pos = String(pos).toLowerCase();
		const p = api.parent(this);
		const ctx = pos === 'beforebegin' || pos === 'afterend' ? p : this;
		const f = api.parse(ctx && api.type(ctx) === S_EL ? ctx : null,
			String(html));
		this._adjacent(pos, f);
	}
	insertAdjacentElement(pos, el) {
		return this._adjacent(String(pos).toLowerCase(), check(el));
	}
	insertAdjacentText(pos, text) {
		this._adjacent(String(pos).toLowerCase(),
			api.createText(String(text)));
	}
	_adjacent(pos, n) {
		const p = api.parent(this);
		switch (pos) {
		case 'beforebegin':
			if (!p)
				return null;
			insert(p, n, this);
			break;
		case 'afterbegin':
			insert(this, n, api.first(this));
			break;
		case 'beforeend':
			insert(this, n, null);
			break;
		case 'afterend':
			if (!p)
				return null;
			insert(p, n, api.next(this));
			break;
		default:
			throw new DOMException('bad position', 'SyntaxError');
		}
		return n;
	}
	matches(sel) {
		try {
			return api.matches(this, String(sel));
		} catch (e) {
			throw new DOMException("'" + sel +
				"' is not a valid selector", 'SyntaxError');
		}
	}
	webkitMatchesSelector(sel) { return this.matches(sel); }
	closest(sel) {
		for (let n = this; n && api.type(n) === S_EL; n = api.parent(n))
			if (n.matches(sel))
				return n;
		return null;
	}
	get style() {
		return this[STYLE] || (this[STYLE] = new Proxy(
			new CSSStyleDeclaration(this), styleHandler));
	}
	set style(v) { this.setAttribute('style', v); }
	get dataset() {
		if (this._dataset)
			return this._dataset;
		const el = this;
		const attr = (p) => 'data-' + p.replace(/[A-Z]/g,
			(c) => '-' + c.toLowerCase());
		const prop = (a) => a.slice(5).replace(/-([a-z])/g,
			(m, c) => c.toUpperCase());
		return (this._dataset = new Proxy({}, {
			get(t, p) {
				if (typeof p !== 'string')
					return undefined;
				const v = el.getAttribute(attr(p));
				return v === null ? undefined : v;
			},
			set(t, p, v) {
				el.setAttribute(attr(p), v);
				return true;
			},
			deleteProperty(t, p) {
				el.removeAttribute(attr(p));
				return true;
			},
			has(t, p) { return el.hasAttribute(attr(p)); },
			ownKeys() {
				return el.getAttributeNames()
					.filter((a) => a.startsWith('data-')).map(prop);
			},
			getOwnPropertyDescriptor(t, p) {
				const v = el.getAttribute(attr(p));
				return v === null ? undefined : { value: v,
					enumerable: true, configurable: true,
					writable: true };
			},
		}));
	}
	getBoundingClientRect() {
		const r = api.rect(this) || [0, 0, 0, 0];
		const v = api.viewport();
		return new DOMRect(r[0] - v[2], r[1] - v[3], r[2], r[3]);
	}
	getClientRects() {
		const r = api.rect(this);
		return list(r ? [this.getBoundingClientRect()] : []);
	}
	get clientWidth() {
		if (this === document.documentElement)
			return api.viewport()[0];
		const r = api.rect(this);
		return r ? Math.round(r[2]) : 0;
	}
	get clientHeight() {
		if (this === document.documentElement)
			return api.viewport()[1];
		const r = api.rect(this);
		return r ? Math.round(r[3]) : 0;
	}
	get clientTop() { return 0; }
	get clientLeft() { return 0; }
	get scrollWidth() {
		if (this === document.documentElement || this === document.body)
			return api.viewport()[0];
		return this.clientWidth;
	}
	get scrollHeight() {
		if (this === document.documentElement || this === document.body)
			return Math.max(api.pageHeight(), api.viewport()[1]);
		return this.clientHeight;
	}
	get scrollTop() {
		return this === document.documentElement || this === document.body ?
			api.viewport()[3] : 0;
	}
	set scrollTop(v) {
		if (this === document.documentElement || this === document.body)
			api.scroll(api.viewport()[2], +v || 0);
	}
	get scrollLeft() { return 0; }
	set scrollLeft(v) {}
	scrollIntoView() {
		const r = api.rect(this);
		if (r)
			api.scroll(0, r[1]);
	}
	scroll() {}
	scrollTo() {}
	scrollBy() {}
	attachShadow() {
		throw new DOMException('no shadow DOM', 'NotSupportedError');
	}
	animate() {
		return { finished: Promise.resolve(), cancel() {}, play() {},
			pause() {}, finish() {}, onfinish: null,
			addEventListener() {} };
	}
	getAnimations() { return []; }
	requestFullscreen() { return Promise.reject(new DOMException('',
		'NotSupportedError')); }
	setPointerCapture() {}
	releasePointerCapture() {}
	hasPointerCapture() { return false; }
}
mixin(Element, childNode);
mixin(Element, parentNode);
for (const ev of ['click', 'dblclick', 'mousedown', 'mouseup', 'mousemove',
	'mouseover', 'mouseout', 'mouseenter', 'mouseleave', 'keydown',
	'keyup', 'keypress', 'focus', 'blur', 'change', 'input', 'submit',
	'reset', 'load', 'error', 'scroll', 'resize', 'contextmenu', 'wheel',
	'select', 'abort', 'play', 'pause', 'ended', 'timeupdate', 'toggle',
	'pointerdown', 'pointerup', 'pointermove', 'touchstart', 'touchend',
	'touchmove', 'animationend', 'transitionend', 'beforeunload',
	'unload', 'popstate', 'hashchange', 'message', 'readystatechange',
	'DOMContentLoaded', 'focusin', 'focusout', 'invalid',
	'loadedmetadata', 'canplay', 'volumechange']) {
	const d = {
		get() {
			const h = this[HANDLERS] && this[HANDLERS][ev];
			if (h !== undefined)
				return h;
			return handlerOf(this, ev);
		},
		set(v) {
			(this[HANDLERS] || (this[HANDLERS] = Object.create(null)))[ev] =
				typeof v === 'function' ? v : null;
		},
		configurable: true,
	};
	Object.defineProperty(Element.prototype, 'on' + ev.toLowerCase(), d);
}

class HTMLElement extends Element {
	get title() { return api.getAttr(this, 'title') || ''; }
	set title(v) { api.setAttr(this, 'title', str(v)); }
	get innerText() { return textOf(this); }
	set innerText(v) { this.textContent = v; }
	get outerText() { return textOf(this); }
	get offsetParent() { return document.body; }
	get offsetTop() {
		const r = api.rect(this);
		return r ? Math.round(r[1]) : 0;
	}
	get offsetLeft() {
		const r = api.rect(this);
		return r ? Math.round(r[0]) : 0;
	}
	get offsetWidth() {
		const r = api.rect(this);
		return r ? Math.round(r[2]) : 0;
	}
	get offsetHeight() {
		const r = api.rect(this);
		return r ? Math.round(r[3]) : 0;
	}
	get isContentEditable() {
		return this.getAttribute('contenteditable') === 'true' ||
			this.getAttribute('contenteditable') === '';
	}
	focus() {
		if (activeElement === this)
			return;
		const old = activeElement;
		activeElement = this;
		if (old)
			dispatch(old, new FocusEvent('blur'));
		dispatch(this, new FocusEvent('focus'));
		dispatch(this, new FocusEvent('focusin', { bubbles: true }));
	}
	blur() {
		if (activeElement !== this)
			return;
		activeElement = null;
		dispatch(this, new FocusEvent('blur'));
	}
	click() {
		if (this.disabled)
			return;
		const ev = new MouseEvent('click', { bubbles: true,
			cancelable: true, view: g, detail: 1 });
		if (dispatch(this, ev))
			activation(this, false);
	}
}
reflect(HTMLElement, ['lang', 'dir', 'accessKey', 'slot', 'nonce',
	'autocapitalize', 'enterKeyHint', 'inputMode', 'contentEditable',
	'draggable']);
reflect(HTMLElement, ['hidden', 'inert', 'spellcheck', 'translate'], 'bool');
Object.defineProperty(HTMLElement.prototype, 'tabIndex', {
	get() {
		const v = parseInt(api.getAttr(this, 'tabindex'), 10);
		return isNaN(v) ? (/^(a|button|input|select|textarea)$/
			.test(api.name(this)) ? 0 : -1) : v;
	},
	set(v) { api.setAttr(this, 'tabindex', String(v | 0)); },
	configurable: true,
});

class SVGElement extends Element {
	get ownerSVGElement() { return this.closest('svg'); }
	get className() {
		const v = api.getAttr(this, 'class') || '';
		return { baseVal: v, animVal: v };
	}
	focus() {}
	blur() {}
	getBBox() {
		const r = api.rect(this) || [0, 0, 0, 0];
		return { x: 0, y: 0, width: r[2], height: r[3] };
	}
}
class SVGSVGElement extends SVGElement {}
class MathMLElement extends Element {}

/* HTML element classes, by tag */
const CLASSES = {};
function html(name, tags, body, base = HTMLElement) {
	const cls = body ? body(base) : class extends base {};
	Object.defineProperty(cls, 'name', { value: name });
	for (const t of tags)
		CLASSES[t] = cls;
	g[name] = cls;
	return cls;
}

function formOf(el) {
	const id = api.getAttr(el, 'form');
	if (id !== null)
		return document.getElementById(id);
	return el.closest('form');
}

html('HTMLAnchorElement', ['a'], (B) => class extends B {
	toString() { return this.href; }
});
reflect(HTMLAnchorElement, ['href'], 'url');
reflect(HTMLAnchorElement, ['target', 'download', 'rel', 'hreflang',
	'type', 'referrerPolicy', 'ping', 'name']);
html('HTMLAreaElement', ['area']);
reflect(HTMLAreaElement, ['href'], 'url');
for (const C of [HTMLAnchorElement, HTMLAreaElement])
	for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname',
		'search', 'hash', 'origin', 'username', 'password'])
		Object.defineProperty(C.prototype, k, {
			get() {
				try {
					return new URL(this.href)[k];
				} catch (e) {
					return '';
				}
			},
			set(v) {
				try {
					const u = new URL(this.href);
					u[k] = v;
					this.setAttribute('href', u.href);
				} catch (e) { /* ignore */ }
			},
			configurable: true,
		});
Object.defineProperty(HTMLAnchorElement.prototype, 'text', {
	get() { return this.textContent; },
	set(v) { this.textContent = v; },
});

html('HTMLImageElement', ['img'], (B) => class extends B {
	get complete() { return true; }
	get naturalWidth() { return this.width; }
	get naturalHeight() { return this.height; }
	get width() {
		const r = api.rect(this);
		return r ? Math.round(r[2]) :
			parseInt(api.getAttr(this, 'width'), 10) || 0;
	}
	set width(v) { api.setAttr(this, 'width', String(v | 0)); }
	get height() {
		const r = api.rect(this);
		return r ? Math.round(r[3]) :
			parseInt(api.getAttr(this, 'height'), 10) || 0;
	}
	set height(v) { api.setAttr(this, 'height', String(v | 0)); }
	get currentSrc() { return this.src; }
	set src(v) {
		api.setAttr(this, 'src', str(v));
		this._loadOnSrc();
	}
	get src() {
		const v = api.getAttr(this, 'src');
		return v === null ? '' : api.resolve(document.baseURI, v) || v;
	}
	_loadOnSrc() {
		/* the engine decodes images when it paints: say "loaded" */
		addTimer(() => dispatch(this, new Event('load')), 0, [], false);
	}
	decode() { return Promise.resolve(); }
});
reflect(HTMLImageElement, ['alt', 'srcset', 'sizes', 'crossOrigin',
	'useMap', 'loading', 'decoding', 'referrerPolicy', 'name']);
reflect(HTMLImageElement, ['isMap'], 'bool');

html('HTMLScriptElement', ['script'], (B) => class extends B {
	get text() { return this.textContent; }
	set text(v) { this.textContent = v; }
});
reflect(HTMLScriptElement, ['src'], 'url');
reflect(HTMLScriptElement, ['type', 'charset', 'crossOrigin', 'integrity',
	'referrerPolicy', 'event', 'htmlFor']);
reflect(HTMLScriptElement, ['async', 'defer', 'noModule'], 'bool');
HTMLScriptElement.supports = (t) => t === 'classic' || t === 'module';

html('HTMLLinkElement', ['link'], (B) => class extends B {
	get relList() { return new DOMTokenList(this, 'rel'); }
	get sheet() { return null; }
});
reflect(HTMLLinkElement, ['href'], 'url');
reflect(HTMLLinkElement, ['rel', 'media', 'type', 'as', 'crossOrigin',
	'hreflang', 'integrity', 'sizes', 'referrerPolicy']);
reflect(HTMLLinkElement, ['disabled'], 'bool');
html('HTMLStyleElement', ['style'], (B) => class extends B {
	get sheet() {
		return { cssRules: [], insertRule: (r, i) => {
			this.textContent += '\n' + r;
			return i || 0;
		}, deleteRule() {} };
	}
});
reflect(HTMLStyleElement, ['media', 'type']);
html('HTMLMetaElement', ['meta']);
reflect(HTMLMetaElement, ['name', 'content', 'charset', 'media',
	['httpEquiv', 'http-equiv']]);
html('HTMLBaseElement', ['base']);
reflect(HTMLBaseElement, ['href'], 'url');
reflect(HTMLBaseElement, ['target']);
html('HTMLTitleElement', ['title'], (B) => class extends B {
	get text() { return this.textContent; }
	set text(v) { this.textContent = v; }
});
html('HTMLHtmlElement', ['html']);
html('HTMLHeadElement', ['head']);
html('HTMLBodyElement', ['body']);
html('HTMLDivElement', ['div']);
html('HTMLSpanElement', ['span']);
html('HTMLParagraphElement', ['p']);
html('HTMLHeadingElement', ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']);
html('HTMLBRElement', ['br']);
html('HTMLHRElement', ['hr']);
html('HTMLPreElement', ['pre', 'listing', 'xmp']);
html('HTMLQuoteElement', ['q', 'blockquote']);
html('HTMLUListElement', ['ul']);
html('HTMLOListElement', ['ol']);
reflect(HTMLOListElement, ['start'], 'int');
reflect(HTMLOListElement, ['reversed'], 'bool');
html('HTMLLIElement', ['li']);
html('HTMLDListElement', ['dl']);
html('HTMLTableElement', ['table'], (B) => class extends B {
	get rows() {
		return list([...this.querySelectorAll('tr')]
			.filter((r) => r.closest('table') === this));
	}
	get tBodies() {
		return list([...this.children]
			.filter((e) => e.localName === 'tbody'));
	}
	get tHead() {
		return [...this.children].find((e) => e.localName === 'thead') ||
			null;
	}
	get tFoot() {
		return [...this.children].find((e) => e.localName === 'tfoot') ||
			null;
	}
	get caption() {
		return [...this.children].find((e) =>
			e.localName === 'caption') || null;
	}
});
html('HTMLTableSectionElement', ['thead', 'tbody', 'tfoot'], (B) =>
	class extends B {
	get rows() { return this.children; }
});
html('HTMLTableRowElement', ['tr'], (B) => class extends B {
	get cells() { return this.children; }
	get rowIndex() {
		const t = this.closest('table');
		return t ? [...t.querySelectorAll('tr')].indexOf(this) : -1;
	}
});
html('HTMLTableCellElement', ['td', 'th'], (B) => class extends B {
	get cellIndex() {
		const p = this.parentElement;
		return p ? [...p.children].indexOf(this) : -1;
	}
});
reflect(HTMLTableCellElement, ['colSpan', 'rowSpan'], 'int');
html('HTMLTableColElement', ['col', 'colgroup']);
html('HTMLTableCaptionElement', ['caption']);
html('HTMLIFrameElement', ['iframe'], (B) => class extends B {
	get contentWindow() { return null; }
	get contentDocument() { return null; }
});
reflect(HTMLIFrameElement, ['src'], 'url');
reflect(HTMLIFrameElement, ['name', 'width', 'height', 'allow',
	'loading', 'srcdoc', 'referrerPolicy']);
html('HTMLCanvasElement', ['canvas'], (B) => class extends B {
	getContext() { return null; }
	toDataURL() { return 'data:,'; }
	toBlob(cb) { cb(null); }
});
reflect(HTMLCanvasElement, ['width', 'height'], 'int');
html('HTMLTemplateElement', ['template'], (B) => class extends B {
	get content() { return api.content(this); }
});
html('HTMLSlotElement', ['slot'], (B) => class extends B {
	assignedNodes() { return []; }
	assignedElements() { return []; }
});
html('HTMLLabelElement', ['label'], (B) => class extends B {
	get control() {
		const f = api.getAttr(this, 'for');
		if (f !== null)
			return document.getElementById(f);
		return this.querySelector('input, select, textarea, button');
	}
	get form() { return formOf(this); }
});
reflect(HTMLLabelElement, [['htmlFor', 'for']]);
html('HTMLFormElement', ['form'], (B) => class extends B {
	get elements() {
		const a = list(query(this, 'input, select, textarea, button, ' +
			'fieldset, output, object', 0));
		a.namedItem = (n) => a.find((e) => e.name === n || e.id === n) ||
			null;
		for (const e of a)
			if (e.name && !(e.name in a))
				a[e.name] = e;
		return a;
	}
	get length() { return this.elements.length; }
	submit() { submitForm(this, null); }
	requestSubmit(submitter) {
		const ev = new SubmitEvent('submit', { bubbles: true,
			cancelable: true, submitter: submitter || null });
		if (dispatch(this, ev))
			submitForm(this, submitter || null);
	}
	reset() {
		if (dispatch(this, new Event('reset', { bubbles: true,
		    cancelable: true })))
			for (const e of this.elements)
				if (e.localName === 'input' &&
				    /^(checkbox|radio)$/.test(e.type))
					e.checked = e.defaultChecked;
				else if (e.localName === 'input' ||
					 e.localName === 'textarea')
					e.value = e.defaultValue;
	}
	checkValidity() { return true; }
	reportValidity() { return true; }
});
reflect(HTMLFormElement, ['action'], 'url');
reflect(HTMLFormElement, ['name', 'target', 'enctype', 'autocomplete',
	['acceptCharset', 'accept-charset']]);
reflect(HTMLFormElement, ['noValidate'], 'bool');
Object.defineProperty(HTMLFormElement.prototype, 'method', {
	get() {
		return (api.getAttr(this, 'method') || 'get').toLowerCase() ===
			'post' ? 'post' : 'get';
	},
	set(v) { api.setAttr(this, 'method', str(v)); },
});

/* form.submit(): GET as a URL, POST through the host's navigation */
function submitForm(form, submitter) {
	const data = new FormData(form, submitter);
	const q = new URLSearchParams();
	for (const [k, v] of data)
		q.append(k, v);
	let action = form.action || document.URL;
	if (form.method === 'post') {
		report('form.submit() with POST is not supported: ' + action);
		return;
	}
	action = action.replace(/[?#].*$/, '') + '?' + q;
	api.navigate(action, false);
}

const formControl = (B) => class extends B {
	get form() { return formOf(this); }
	get labels() {
		const a = this.id ? [...document.querySelectorAll('label[for="' +
			this.id.replace(/["\\]/g, '\\$&') + '"]')] : [];
		const l = this.closest('label');
		if (l && !a.includes(l))
			a.push(l);
		return list(a);
	}
	get validity() {
		return { valid: true, valueMissing: false, typeMismatch: false,
			patternMismatch: false, tooLong: false, tooShort: false,
			rangeUnderflow: false, rangeOverflow: false,
			stepMismatch: false, badInput: false, customError: false };
	}
	get validationMessage() { return ''; }
	get willValidate() { return true; }
	checkValidity() { return true; }
	reportValidity() { return true; }
	setCustomValidity() {}
};
html('HTMLInputElement', ['input'], (B) => class extends formControl(B) {
	get type() {
		const t = (api.getAttr(this, 'type') || 'text').toLowerCase();
		return /^(hidden|text|search|tel|url|email|password|date|month|week|time|datetime-local|number|range|color|checkbox|radio|file|submit|image|reset|button)$/
			.test(t) ? t : 'text';
	}
	set type(v) { api.setAttr(this, 'type', str(v)); }
	get value() {
		const v = api.getAttr(this, 'value');
		if (v === null)
			return /^(checkbox|radio)$/.test(this.type) ? 'on' : '';
		return v;
	}
	set value(v) {
		api.setAttr(this, 'value', v === null ? '' : str(v));
	}
	get defaultValue() { return api.getAttr(this, 'value') || ''; }
	set defaultValue(v) { api.setAttr(this, 'value', str(v)); }
	get checked() { return api.getAttr(this, 'checked') !== null; }
	set checked(v) {
		if (v) {
			if (this.type === 'radio' && this.name) {
				const f = this.form || document;
				for (const r of f.querySelectorAll('input[type=radio]'))
					if (r !== this && r.name === this.name)
						api.removeAttr(r, 'checked');
			}
			api.setAttr(this, 'checked', '');
		} else {
			api.removeAttr(this, 'checked');
		}
	}
	get defaultChecked() { return this.checked; }
	get valueAsNumber() { return parseFloat(this.value); }
	set valueAsNumber(v) { this.value = String(v); }
	get files() { return list([]); }
	get selectionStart() { return this.value.length; }
	get selectionEnd() { return this.value.length; }
	select() {}
	setSelectionRange() {}
	setRangeText() {}
	stepUp() {}
	stepDown() {}
	showPicker() {}
	click() {
		if (this.disabled)
			return;
		const t = this.type;
		const was = this.checked;
		if (t === 'checkbox')
			this.checked = !was;
		else if (t === 'radio')
			this.checked = true;
		const ev = new MouseEvent('click', { bubbles: true,
			cancelable: true, view: g, detail: 1 });
		if (!dispatch(this, ev)) {
			if (t === 'checkbox' || t === 'radio')
				this.checked = was;
			return;
		}
		if ((t === 'checkbox' || t === 'radio') && was !== this.checked) {
			dispatch(this, new InputEvent('input', { bubbles: true }));
			dispatch(this, new Event('change', { bubbles: true }));
		} else {
			activation(this, false);
		}
	}
});
reflect(HTMLInputElement, ['name', 'placeholder', 'autocomplete',
	'accept', 'alt', 'pattern', 'min', 'max', 'step', 'dirName',
	'inputMode', 'list']);
reflect(HTMLInputElement, ['src', 'formAction'], 'url');
reflect(HTMLInputElement, ['disabled', 'required', 'readOnly', 'multiple',
	'autofocus', 'formNoValidate'], 'bool');
reflect(HTMLInputElement, ['size', 'maxLength', 'minLength'], 'int');
html('HTMLTextAreaElement', ['textarea'], (B) => class extends formControl(B) {
	get type() { return 'textarea'; }
	get value() { return this.textContent; }
	set value(v) { this.textContent = v === null ? '' : str(v); }
	get defaultValue() { return this.textContent; }
	set defaultValue(v) { this.textContent = v; }
	get textLength() { return this.value.length; }
	get selectionStart() { return this.value.length; }
	get selectionEnd() { return this.value.length; }
	select() {}
	setSelectionRange() {}
});
reflect(HTMLTextAreaElement, ['name', 'placeholder', 'autocomplete',
	'wrap', 'dirName']);
reflect(HTMLTextAreaElement, ['disabled', 'required', 'readOnly',
	'autofocus'], 'bool');
reflect(HTMLTextAreaElement, ['rows', 'cols', 'maxLength', 'minLength'],
	'int');
html('HTMLButtonElement', ['button'], (B) => class extends formControl(B) {
	get type() {
		const t = (api.getAttr(this, 'type') || '').toLowerCase();
		return t === 'button' || t === 'reset' ? t : 'submit';
	}
	set type(v) { api.setAttr(this, 'type', str(v)); }
});
reflect(HTMLButtonElement, ['name', 'value']);
reflect(HTMLButtonElement, ['formAction'], 'url');
reflect(HTMLButtonElement, ['disabled', 'autofocus', 'formNoValidate'],
	'bool');
html('HTMLSelectElement', ['select'], (B) => class extends formControl(B) {
	get options() {
		const a = list(query(this, 'option', 0));
		a.selectedIndex = this.selectedIndex;
		a.add = (o, before) => this.add(o, before);
		a.remove = (i) => this.remove(i);
		return a;
	}
	get length() { return this.options.length; }
	get type() { return this.multiple ? 'select-multiple' : 'select-one'; }
	get selectedOptions() {
		return list(this.options.filter((o) => o.selected));
	}
	get selectedIndex() {
		const o = query(this, 'option', 0);
		const i = o.findIndex((x) => x.hasAttribute('selected'));
		return i >= 0 ? i : o.length && !this.multiple ? 0 : -1;
	}
	set selectedIndex(i) {
		this.options.forEach((o, k) => {
			if (k === i)
				api.setAttr(o, 'selected', '');
			else if (!this.multiple)
				api.removeAttr(o, 'selected');
		});
	}
	get value() {
		const o = this.options[this.selectedIndex];
		return o ? o.value : '';
	}
	set value(v) {
		this.selectedIndex = this.options.findIndex((o) =>
			o.value === String(v));
	}
	item(i) { return this.options[i] || null; }
	namedItem(n) {
		return this.options.find((o) => o.id === n || o.name === n) ||
			null;
	}
	add(o, before) {
		const ref = typeof before === 'number' ? this.options[before] :
			before;
		this.insertBefore(o, ref || null);
	}
	remove(i) {
		if (i === undefined)
			return childNode.remove.call(this);
		const o = this.options[i];
		if (o)
			o.remove();
	}
});
reflect(HTMLSelectElement, ['name', 'autocomplete']);
reflect(HTMLSelectElement, ['disabled', 'required', 'multiple',
	'autofocus'], 'bool');
reflect(HTMLSelectElement, ['size'], 'int');
html('HTMLOptionElement', ['option'], (B) => class extends B {
	get value() {
		const v = api.getAttr(this, 'value');
		return v === null ? this.text : v;
	}
	set value(v) { api.setAttr(this, 'value', str(v)); }
	get text() { return textOf(this).replace(/\s+/g, ' ').trim(); }
	set text(v) { this.textContent = v; }
	get label() { return api.getAttr(this, 'label') || this.text; }
	get selected() {
		const s = this.closest('select');
		if (!s)
			return this.hasAttribute('selected');
		return s.options[s.selectedIndex] === this ||
			(s.multiple && this.hasAttribute('selected'));
	}
	set selected(v) {
		const s = this.closest('select');
		if (v && s && !s.multiple)
			for (const o of s.options)
				api.removeAttr(o, 'selected');
		if (v)
			api.setAttr(this, 'selected', '');
		else
			api.removeAttr(this, 'selected');
	}
	get defaultSelected() { return this.hasAttribute('selected'); }
	get index() {
		const s = this.closest('select');
		return s ? s.options.indexOf(this) : 0;
	}
	get form() { return formOf(this); }
});
reflect(HTMLOptionElement, ['disabled'], 'bool');
html('HTMLOptGroupElement', ['optgroup']);
reflect(HTMLOptGroupElement, ['label']);
reflect(HTMLOptGroupElement, ['disabled'], 'bool');
html('HTMLFieldSetElement', ['fieldset'], (B) => class extends formControl(B) {
	get elements() {
		return list(query(this, 'input, select, textarea, button', 0));
	}
});
reflect(HTMLFieldSetElement, ['name']);
reflect(HTMLFieldSetElement, ['disabled'], 'bool');
html('HTMLLegendElement', ['legend']);
html('HTMLOutputElement', ['output'], (B) => class extends formControl(B) {
	get value() { return this.textContent; }
	set value(v) { this.textContent = v; }
});
html('HTMLProgressElement', ['progress']);
html('HTMLMeterElement', ['meter']);
for (const C of [HTMLProgressElement, HTMLMeterElement])
	for (const k of ['value', 'max', 'min', 'low', 'high', 'optimum'])
		Object.defineProperty(C.prototype, k, {
			get() { return parseFloat(api.getAttr(this, k)) || 0; },
			set(v) { api.setAttr(this, k, String(+v)); },
		});
html('HTMLDetailsElement', ['details']);
reflect(HTMLDetailsElement, ['open'], 'bool');
html('HTMLDialogElement', ['dialog'], (B) => class extends B {
	show() { api.setAttr(this, 'open', ''); }
	showModal() { api.setAttr(this, 'open', ''); }
	close(v) {
		if (v !== undefined)
			this.returnValue = v;
		api.removeAttr(this, 'open');
		dispatch(this, new Event('close'));
	}
});
reflect(HTMLDialogElement, ['open'], 'bool');
/* Media: the engine's players (SOIE_MEDIA_* operations) */
const M_PLAY = 0, M_PAUSE = 1, M_SEEK = 3, M_VOLUME = 4, M_MUTE = 5;
html('HTMLMediaElement', [], (B) => class extends B {
	get _st() { return api.playerState(this); }
	get paused() {
		const s = this._st;
		return !s || s.paused;
	}
	get ended() {
		const s = this._st;
		return !!(s && s.ended);
	}
	get currentTime() {
		const s = this._st;
		return s ? s.time : this._seekTo || 0;
	}
	set currentTime(v) {
		this._seekTo = +v || 0;
		if (api.playerState(this))
			api.player(this, M_SEEK, this._seekTo);
		addTimer(() => {
			dispatch(this, new Event('seeking'));
			dispatch(this, new Event('seeked'));
		}, 0, [], false);
	}
	get duration() {
		const s = this._st;
		return s && s.duration > 0 ? s.duration : NaN;
	}
	get readyState() {
		const s = this._st;
		return s && (s.duration > 0 || s.width) ? 4 : 0;
	}
	get networkState() { return this._st ? 1 : 0; }
	get videoWidth() {
		const s = this._st;
		return s ? s.width : 0;
	}
	get videoHeight() {
		const s = this._st;
		return s ? s.height : 0;
	}
	get volume() { return this._volume === undefined ? 1 : this._volume; }
	set volume(v) {
		this._volume = Math.min(1, Math.max(0, +v));
		api.player(this, M_VOLUME, this._volume);
		dispatch(this, new Event('volumechange'));
	}
	get muted() { return this.hasAttribute('muted'); }
	set muted(v) {
		this.toggleAttribute('muted', !!v);
		api.player(this, M_MUTE, v ? 1 : 0);
		dispatch(this, new Event('volumechange'));
	}
	get playbackRate() { return 1; }
	set playbackRate(v) {}
	get error() {
		const s = this._st;
		return s && s.error ? { code: 4, message: 'cannot play' } : null;
	}
	get buffered() {
		const d = this.duration;
		return { length: isNaN(d) ? 0 : 1, start: () => 0,
			end: () => isNaN(d) ? 0 : d };
	}
	get seekable() { return this.buffered; }
	get currentSrc() { return this.src; }
	canPlayType(t) { return api.canPlay(String(t)) ? 'maybe' : ''; }
	load() {}
	play() {
		if (!api.player(this, M_PLAY, 0))
			return Promise.reject(new DOMException('cannot play',
				'NotSupportedError'));
		if (this._seekTo)
			api.player(this, M_SEEK, this._seekTo);
		this._seekTo = 0;
		dispatch(this, new Event('play'));
		return Promise.resolve();
	}
	pause() {
		api.player(this, M_PAUSE, 0);
	}
	fastSeek(t) { this.currentTime = t; }
});
reflect(HTMLMediaElement, ['src'], 'url');
reflect(HTMLMediaElement, ['preload', 'crossOrigin']);
reflect(HTMLMediaElement, ['autoplay', 'loop', 'controls',
	['defaultMuted', 'muted']], 'bool');
html('HTMLVideoElement', ['video'], null, HTMLMediaElement);
reflect(HTMLVideoElement, ['poster'], 'url');
reflect(HTMLVideoElement, ['width', 'height'], 'int');
reflect(HTMLVideoElement, ['playsInline'], 'bool');
html('HTMLAudioElement', ['audio'], null, HTMLMediaElement);
html('HTMLSourceElement', ['source']);
reflect(HTMLSourceElement, ['src'], 'url');
reflect(HTMLSourceElement, ['type', 'srcset', 'sizes', 'media']);
html('HTMLTrackElement', ['track']);
html('HTMLPictureElement', ['picture']);
html('HTMLObjectElement', ['object']);
html('HTMLEmbedElement', ['embed']);
html('HTMLMapElement', ['map']);
html('HTMLDataElement', ['data']);
html('HTMLTimeElement', ['time']);
reflect(HTMLTimeElement, ['dateTime', 'datetime']);
html('HTMLModElement', ['ins', 'del']);
html('HTMLMenuElement', ['menu']);
html('HTMLUnknownElement', []);
g.HTMLElement = HTMLElement;

/* Custom elements: a registry; defined classes upgrade new elements
 * made by createElement() and the parser's ones at define(). */
const customs = new Map();
const customElements = {
	define(name, cls) {
		if (customs.has(name))
			throw new DOMException(name + ' already defined',
				'NotSupportedError');
		customs.set(name, cls);
		for (const el of document.querySelectorAll(name))
			upgrade(el, cls);
		const w = whenDefined.get(name);
		if (w)
			w.forEach((r) => r(cls));
	},
	get(name) { return customs.get(name); },
	whenDefined(name) {
		if (customs.has(name))
			return Promise.resolve(customs.get(name));
		return new Promise((r) => {
			if (!whenDefined.has(name))
				whenDefined.set(name, []);
			whenDefined.get(name).push(r);
		});
	},
	upgrade() {},
};
const whenDefined = new Map();
function upgrade(el, cls) {
	if (Object.getPrototypeOf(el) === cls.prototype)
		return;
	Object.setPrototypeOf(el, cls.prototype);
	try {
		/* run the class's constructor body on the existing element */
		const saved = constructing;
		constructing = el;
		Reflect.construct(cls, [], cls);
		constructing = saved;
		if (el.isConnected && el.connectedCallback)
			el.connectedCallback();
	} catch (e) {
		report(e);
	}
}
let constructing = null;
/* HTMLElement's "constructor" for custom element classes */
const HTMLElementCtor = function () {
	if (constructing) {
		const el = constructing;
		constructing = null;
		return el;
	}
	const name = [...customs].find(([, c]) => c === new.target);
	const el = api.createElement(NS_HTML, name ? name[0] : 'div');
	Object.setPrototypeOf(el, new.target.prototype);
	return el;
};
HTMLElementCtor.prototype = HTMLElement.prototype;
Object.defineProperty(HTMLElement.prototype, 'constructor', {
	value: HTMLElementCtor, writable: true, configurable: true });
g.HTMLElement = HTMLElementCtor;

api.protoFor = function (type, name, ns) {
	switch (type) {
	case S_DOC: return Document.prototype;
	case S_FRAG: return DocumentFragment.prototype;
	case S_DOCTYPE: return DocumentType.prototype;
	case S_TEXT: return Text.prototype;
	case S_COMMENT: return Comment.prototype;
	}
	if (ns === NS_SVG)
		return name === 'svg' ? SVGSVGElement.prototype :
			SVGElement.prototype;
	if (ns !== NS_HTML)
		return ns === 3 ? MathMLElement.prototype : Element.prototype;
	const c = customs.get(name);
	if (c)
		return c.prototype;
	return (CLASSES[name] || (name.includes('-') ? HTMLElement :
		/^(abbr|address|article|aside|b|bdi|bdo|cite|code|dd|dfn|dt|em|figcaption|figure|footer|header|hgroup|i|kbd|main|mark|nav|noscript|rp|rt|ruby|s|samp|search|section|small|strong|sub|summary|sup|u|var|wbr|center|big|tt|acronym|strike|nobr|noembed|noframes|plaintext|marquee)$/
		.test(name) ? HTMLElement : HTMLUnknownElement)).prototype;
};

/* Geometry */
class DOMRectReadOnly {
	constructor(x = 0, y = 0, width = 0, height = 0) {
		this.x = x;
		this.y = y;
		this.width = width;
		this.height = height;
	}
	get top() { return Math.min(this.y, this.y + this.height); }
	get left() { return Math.min(this.x, this.x + this.width); }
	get right() { return Math.max(this.x, this.x + this.width); }
	get bottom() { return Math.max(this.y, this.y + this.height); }
	toJSON() {
		return { x: this.x, y: this.y, width: this.width,
			height: this.height, top: this.top, left: this.left,
			right: this.right, bottom: this.bottom };
	}
	static fromRect(r = {}) {
		return new this(r.x, r.y, r.width, r.height);
	}
}
class DOMRect extends DOMRectReadOnly {}

/* Document */

let readyState = 'loading';
let activeElement = null;

class Document extends Node {
	get documentElement() { return api.root(this); }
	get head() {
		return query(this, 'html > head', 1)[0] || null;
	}
	get body() {
		return query(this, 'html > body, html > frameset', 1)[0] || null;
	}
	set body(b) {
		const old = this.body, html = this.documentElement;
		if (old)
			html.replaceChild(b, old);
		else if (html)
			html.appendChild(b);
	}
	get title() {
		const t = query(this, 'title', 1)[0];
		return t ? textOf(t).replace(/[ \t\n\f\r]+/g, ' ').trim() : '';
	}
	set title(v) {
		let t = query(this, 'title', 1)[0];
		if (!t) {
			const h = this.head;
			if (!h)
				return;
			t = api.createElement(NS_HTML, 'title');
			api.insert(h, t, null);
		}
		t.textContent = v;
	}
	get URL() { return location.href; }
	get documentURI() { return location.href; }
	get baseURI() {
		const b = query(this, 'base[href]', 1)[0];
		return b ? api.resolve(location.href, api.getAttr(b, 'href')) ||
			location.href : location.href;
	}
	get location() { return location; }
	set location(v) { location.href = v; }
	get domain() { return location.hostname; }
	get referrer() { return ''; }
	get cookie() { return api.cookie(); }
	set cookie(v) { api.cookie(String(v)); }
	get readyState() { return readyState; }
	get characterSet() { return 'UTF-8'; }
	get charset() { return 'UTF-8'; }
	get inputEncoding() { return 'UTF-8'; }
	get contentType() { return 'text/html'; }
	get compatMode() {
		const d = api.first(this);
		return d && api.type(d) === S_DOCTYPE ? 'CSS1Compat' : 'BackCompat';
	}
	get doctype() {
		for (let k = api.first(this); k; k = api.next(k))
			if (api.type(k) === S_DOCTYPE)
				return k;
		return null;
	}
	get defaultView() { return g; }
	get activeElement() { return activeElement || this.body; }
	get currentScript() { return api.currentScript(); }
	get hidden() { return false; }
	get visibilityState() { return 'visible'; }
	get fullscreenElement() { return null; }
	get scrollingElement() { return this.documentElement; }
	get forms() { return list(query(this, 'form', 0)); }
	get images() { return list(query(this, 'img', 0)); }
	get links() { return list(query(this, 'a[href], area[href]', 0)); }
	get scripts() { return list(query(this, 'script', 0)); }
	get styleSheets() { return list([]); }
	get implementation() {
		return {
			hasFeature: () => true,
			createHTMLDocument() {
				throw new DOMException('one document per page',
					'NotSupportedError');
			},
		};
	}
	hasFocus() { return true; }
	getElementById(id) {
		return query(this, '#' + escapeIdent(id), 1)[0] || null;
	}
	getElementsByName(n) {
		return list(query(this, '[name="' + String(n)
			.replace(/["\\]/g, '\\$&') + '"]', 0));
	}
	createElement(name, opts) {
		name = String(name);
		if (!/^[a-zA-Z][^\s\/>\0]*$/.test(name))
			throw new DOMException('bad element name',
				'InvalidCharacterError');
		const el = api.createElement(NS_HTML, name.toLowerCase());
		const is = opts && (typeof opts === 'string' ? opts : opts.is);
		const c = customs.get(name.toLowerCase()) || (is && customs.get(is));
		if (c)
			upgrade(el, c);
		return el;
	}
	createElementNS(ns, qname) {
		const i = NS_URI.indexOf(ns);
		const local = String(qname).replace(/^[^:]*:/, '');
		return api.createElement(i > 0 ? i : 0, local);
	}
	createTextNode(s) { return api.createText(str(s)); }
	createComment(s) { return api.createComment(str(s)); }
	createCDATASection(s) { return api.createText(str(s)); }
	createDocumentFragment() { return api.createFragment(); }
	createAttribute(n) { return new Attr(null, String(n).toLowerCase(), '', 0); }
	createEvent(type) {
		const C = { mouseevent: MouseEvent, mouseevents: MouseEvent,
			keyboardevent: KeyboardEvent, keyboardevents: KeyboardEvent,
			customevent: CustomEvent, uievent: UIEvent,
			uievents: UIEvent, focusevent: FocusEvent }
			[String(type).toLowerCase()] || Event;
		const e = new C('');
		return e;
	}
	createRange() { return new Range(); }
	createTreeWalker(root, what = 0xffffffff, filter = null) {
		return new TreeWalker(root, what, filter);
	}
	createNodeIterator(root, what = 0xffffffff, filter = null) {
		return new TreeWalker(root, what, filter);
	}
	importNode(n, deep) { return api.clone(check(n), !!deep); }
	adoptNode(n) {
		if (api.parent(n))
			api.remove(n);
		return n;
	}
	write(...s) { api.write(s.join('')); }
	writeln(...s) { api.write(s.join('') + '\n'); }
	open() { return this; }
	close() {}
	elementFromPoint() { return null; }
	elementsFromPoint() { return []; }
	getSelection() { return g.getSelection(); }
	execCommand() { return false; }
	queryCommandSupported() { return false; }
	exitFullscreen() { return Promise.resolve(); }
	startViewTransition(cb) {
		const p = Promise.resolve().then(() => cb && cb());
		return { finished: p, ready: p, updateCallbackDone: p,
			skipTransition() {} };
	}
}
mixin(Document, parentNode);
for (const ev of ['readystatechange', 'DOMContentLoaded', 'click',
	'keydown', 'keyup', 'scroll', 'visibilitychange', 'input', 'change',
	'submit', 'mousedown', 'mouseup', 'focusin', 'focusout'])
	Object.defineProperty(Document.prototype, 'on' + ev.toLowerCase(), {
		get() {
			return (this[HANDLERS] && this[HANDLERS][ev]) || null;
		},
		set(v) {
			(this[HANDLERS] || (this[HANDLERS] = Object.create(null)))[ev] =
				typeof v === 'function' ? v : null;
		},
	});

/* Tree walking */
class TreeWalker {
	constructor(root, what, filter) {
		this.root = root;
		this.whatToShow = what;
		this.filter = filter;
		this.currentNode = root;
		this._started = false;
	}
	_ok(n) {
		if (!(this.whatToShow & (1 << (n.nodeType - 1))))
			return 3;
		const f = this.filter;
		if (!f)
			return 1;
		return typeof f === 'function' ? f(n) : f.acceptNode(n);
	}
	_following(n) {
		const f = api.first(n);
		if (f)
			return f;
		for (; n && n !== this.root; n = api.parent(n)) {
			const s = api.next(n);
			if (s)
				return s;
		}
		return null;
	}
	nextNode() {
		let n = this.currentNode;
		while ((n = this._following(n)))
			if (this._ok(n) === 1)
				return (this.currentNode = n);
		return null;
	}
	previousNode() {
		let n = this.currentNode;
		while (n && n !== this.root) {
			let p = api.prev(n);
			if (p) {
				while (api.last(p))
					p = api.last(p);
				n = p;
			} else {
				n = api.parent(n);
			}
			if (n && this._ok(n) === 1)
				return (this.currentNode = n);
		}
		return null;
	}
	parentNode() {
		for (let n = this.currentNode; n && n !== this.root; ) {
			n = api.parent(n);
			if (n && this._ok(n) === 1)
				return (this.currentNode = n);
		}
		return null;
	}
	firstChild() {
		for (let n = api.first(this.currentNode); n; n = api.next(n))
			if (this._ok(n) === 1)
				return (this.currentNode = n);
		return null;
	}
	lastChild() {
		for (let n = api.last(this.currentNode); n; n = api.prev(n))
			if (this._ok(n) === 1)
				return (this.currentNode = n);
		return null;
	}
	nextSibling() {
		for (let n = api.next(this.currentNode); n; n = api.next(n))
			if (this._ok(n) === 1)
				return (this.currentNode = n);
		return null;
	}
	previousSibling() {
		for (let n = api.prev(this.currentNode); n; n = api.prev(n))
			if (this._ok(n) === 1)
				return (this.currentNode = n);
		return null;
	}
	detach() {}
}
const NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3,
	SHOW_ALL: 0xffffffff, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2,
	SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_COMMENT: 128,
	SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512,
	SHOW_DOCUMENT_FRAGMENT: 1024 };

/* A minimal Range: enough for createContextualFragment and friends */
class Range {
	constructor() {
		this.startContainer = this.endContainer = document;
		this.startOffset = this.endOffset = 0;
		this.collapsed = true;
		this.commonAncestorContainer = document;
	}
	setStart(n, o) { this.startContainer = n; this.startOffset = o; }
	setEnd(n, o) { this.endContainer = n; this.endOffset = o; }
	setStartBefore(n) { this.startContainer = n; }
	setStartAfter(n) { this.startContainer = n; }
	setEndBefore(n) { this.endContainer = n; }
	setEndAfter(n) { this.endContainer = n; }
	selectNode(n) { this.startContainer = this.endContainer = n; }
	selectNodeContents(n) { this.startContainer = this.endContainer = n; }
	collapse() { this.collapsed = true; }
	cloneRange() { return Object.assign(new Range(), this); }
	detach() {}
	createContextualFragment(html) {
		const c = this.startContainer;
		return api.parse(c && api.type(c) === S_EL ? c : null,
			String(html));
	}
	getBoundingClientRect() { return new DOMRect(); }
	getClientRects() { return list([]); }
	toString() { return ''; }
	deleteContents() {}
	extractContents() { return api.createFragment(); }
	cloneContents() { return api.createFragment(); }
	insertNode(n) {
		const c = this.startContainer;
		if (c && api.type(c) !== S_TEXT)
			c.insertBefore(n, c.childNodes[this.startOffset] || null);
	}
	surroundContents() {}
}

/* Location */
let href = api.url;
const location = {
	get href() { return href; },
	set href(v) { navigate(String(v), false); },
	assign(v) { navigate(String(v), false); },
	replace(v) { navigate(String(v), true); },
	reload() { api.navigate(href, true); },
	toString() { return href; },
	get ancestorOrigins() { return list([]); },
};
for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname',
	'search', 'hash', 'origin'])
	Object.defineProperty(location, k, {
		get() {
			try {
				return new URL(href)[k];
			} catch (e) {
				return '';
			}
		},
		set(v) {
			const u = new URL(href);
			u[k] = v;
			navigate(u.href, false);
		},
		enumerable: true,
	});

function navigate(url, replace) {
	const to = api.resolve(href, url) || url;
	const strip = (u) => u.replace(/#.*$/, '');
	if (strip(to) === strip(href) && to.includes('#')) {
		/* a fragment: no load, a hashchange */
		const old = href;
		href = to;
		api.navigate(to, replace);
		if (old !== to)
			addTimer(() => dispatch(g, new HashChangeEvent('hashchange',
				{ oldURL: old, newURL: to })), 0, [], false);
		return;
	}
	api.navigate(to, replace);
}

/* History: pushState changes the URL scripts see, not the host's */
const states = [{ state: null, url: href }];
let stateIndex = 0;
const history = {
	get length() { return states.length; },
	get state() { return states[stateIndex].state; },
	get scrollRestoration() { return 'auto'; },
	set scrollRestoration(v) {},
	pushState(state, title, url) {
		if (url !== undefined && url !== null)
			href = api.resolve(href, String(url)) || href;
		states.splice(stateIndex + 1);
		states.push({ state, url: href });
		stateIndex++;
	},
	replaceState(state, title, url) {
		if (url !== undefined && url !== null)
			href = api.resolve(href, String(url)) || href;
		states[stateIndex] = { state, url: href };
	},
	go(n) {
		n = n | 0;
		const to = stateIndex + n;
		if (!n || to < 0 || to >= states.length)
			return;
		stateIndex = to;
		href = states[to].url;
		addTimer(() => dispatch(g, new PopStateEvent('popstate',
			{ state: states[to].state })), 0, [], false);
	},
	back() { this.go(-1); },
	forward() { this.go(1); },
};

/* URL and friends */

function percentDecode(s) {
	try {
		return decodeURIComponent(s.replace(/\+/g, ' '));
	} catch (e) {
		return s;
	}
}
function formEncode(s) {
	return encodeURIComponent(s).replace(/%20/g, '+')
		.replace(/[!'()~]/g, (c) => '%' + c.charCodeAt(0).toString(16)
		.toUpperCase());
}

class URLSearchParams {
	constructor(init = '') {
		this._list = [];
		this._url = null;
		if (init instanceof URLSearchParams) {
			this._list = init._list.map((x) => x.slice());
		} else if (typeof init === 'object' && init !== null) {
			if (typeof init[Symbol.iterator] === 'function')
				for (const [k, v] of init)
					this._list.push([String(k), String(v)]);
			else
				for (const k of Object.keys(init))
					this._list.push([k, String(init[k])]);
		} else {
			this._parse(String(init));
		}
	}
	_parse(s) {
		this._list = [];
		for (const part of s.replace(/^\?/, '').split('&'))
			if (part) {
				const i = part.indexOf('=');
				this._list.push(i < 0 ? [percentDecode(part), ''] :
					[percentDecode(part.slice(0, i)),
					 percentDecode(part.slice(i + 1))]);
			}
	}
	_update() {
		if (this._url)
			this._url._search = this._list.length ? '?' + this : '';
	}
	get size() { return this._list.length; }
	append(k, v) {
		this._list.push([String(k), String(v)]);
		this._update();
	}
	delete(k) {
		this._list = this._list.filter((x) => x[0] !== String(k));
		this._update();
	}
	get(k) {
		const x = this._list.find((y) => y[0] === String(k));
		return x ? x[1] : null;
	}
	getAll(k) {
		return this._list.filter((x) => x[0] === String(k)).map((x) => x[1]);
	}
	has(k) { return this._list.some((x) => x[0] === String(k)); }
	set(k, v) {
		k = String(k);
		const i = this._list.findIndex((x) => x[0] === k);
		if (i < 0) {
			this._list.push([k, String(v)]);
		} else {
			this._list[i][1] = String(v);
			this._list = this._list.filter((x, j) => j <= i || x[0] !== k);
		}
		this._update();
	}
	sort() {
		this._list.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0);
		this._update();
	}
	forEach(fn, self) {
		for (const [k, v] of this._list)
			fn.call(self, v, k, this);
	}
	keys() { return this._list.map((x) => x[0])[Symbol.iterator](); }
	values() { return this._list.map((x) => x[1])[Symbol.iterator](); }
	entries() {
		return this._list.map((x) => x.slice())[Symbol.iterator]();
	}
	[Symbol.iterator]() { return this.entries(); }
	toString() {
		return this._list.map(([k, v]) => formEncode(k) + '=' +
			formEncode(v)).join('&');
	}
}

const URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/]*)(?::([^@\/]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
const DEFAULT_PORT = { 'http:': '80', 'https:': '443', 'ws:': '80',
	'wss:': '443', 'ftp:': '21' };

class URL {
	constructor(url, base) {
		url = String(url);
		let abs;
		if (base !== undefined)
			abs = api.resolve(new URL(String(base)).href, url);
		else if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url))
			abs = api.resolve(url, url) || url;
		if (!abs || !URL_RE.test(abs))
			throw new TypeError("Invalid URL: '" + url + "'");
		this._set(abs);
	}
	_set(abs) {
		const m = URL_RE.exec(abs);
		this._protocol = m[1].toLowerCase();
		this._username = m[2] || '';
		this._password = m[3] || '';
		this._hostname = (m[4] || '').toLowerCase();
		this._port = m[5] && m[5] !== DEFAULT_PORT[this._protocol] ?
			m[5] : '';
		this._pathname = m[6] || (m[4] !== undefined ? '/' : '');
		this._search = m[7] && m[7] !== '?' ? m[7] : '';
		this._hash = m[8] && m[8] !== '#' ? m[8] : '';
		this._special = m[4] !== undefined;
		this._params = null;
	}
	get href() {
		return this._protocol + (this._special ? '//' +
			(this._username ? this._username + (this._password ? ':' +
			this._password : '') + '@' : '') + this.host : '') +
			this._pathname + this._search + this._hash;
	}
	set href(v) { this._set(new URL(v).href); }
	get protocol() { return this._protocol; }
	set protocol(v) { this._protocol = String(v).replace(/:?$/, ':'); }
	get username() { return this._username; }
	set username(v) { this._username = String(v); }
	get password() { return this._password; }
	set password(v) { this._password = String(v); }
	get host() {
		return this._hostname + (this._port ? ':' + this._port : '');
	}
	set host(v) {
		const m = /^([^:]*)(?::(\d*))?$/.exec(String(v));
		if (m) {
			this._hostname = m[1].toLowerCase();
			this._port = m[2] || '';
		}
	}
	get hostname() { return this._hostname; }
	set hostname(v) { this._hostname = String(v).toLowerCase(); }
	get port() { return this._port; }
	set port(v) { this._port = String(v); }
	get pathname() { return this._pathname; }
	set pathname(v) {
		v = String(v);
		this._pathname = v.startsWith('/') ? v : '/' + v;
	}
	get search() { return this._search; }
	set search(v) {
		v = String(v);
		this._search = v === '' || v === '?' ? '' :
			v.startsWith('?') ? v : '?' + v;
		if (this._params)
			this._params._parse(this._search);
	}
	get searchParams() {
		if (!this._params) {
			this._params = new URLSearchParams(this._search);
			this._params._url = this;
		}
		return this._params;
	}
	get hash() { return this._hash; }
	set hash(v) {
		v = String(v);
		this._hash = v === '' || v === '#' ? '' :
			v.startsWith('#') ? v : '#' + v;
	}
	get origin() {
		return this._special ? this._protocol + '//' + this.host : 'null';
	}
	toString() { return this.href; }
	toJSON() { return this.href; }
	static canParse(u, b) {
		try {
			new URL(u, b);
			return true;
		} catch (e) {
			return false;
		}
	}
	static parse(u, b) {
		try {
			return new URL(u, b);
		} catch (e) {
			return null;
		}
	}
	static createObjectURL() { return 'blob:' + location.origin + '/0'; }
	static revokeObjectURL() {}
}

/* Text encoding */
class TextEncoder {
	get encoding() { return 'utf-8'; }
	encode(s = '') {
		const u = unescape(encodeURIComponent(String(s)));
		const a = new Uint8Array(u.length);
		for (let i = 0; i < u.length; i++)
			a[i] = u.charCodeAt(i);
		return a;
	}
	encodeInto(s, dest) {
		const a = this.encode(s);
		const n = Math.min(a.length, dest.length);
		dest.set(a.subarray(0, n));
		return { read: s.length, written: n };
	}
}
class TextDecoder {
	constructor(label = 'utf-8') { this.encoding = String(label).toLowerCase(); }
	decode(buf) {
		if (!buf)
			return '';
		const a = buf instanceof ArrayBuffer ? new Uint8Array(buf) :
			new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
		let s = '';
		for (let i = 0; i < a.length; i += 8192)
			s += String.fromCharCode.apply(null, a.subarray(i, i + 8192));
		if (this.encoding !== 'utf-8' && this.encoding !== 'utf8')
			return s;
		try {
			return decodeURIComponent(escape(s));
		} catch (e) {
			return s;
		}
	}
}

/* Base64: binary strings */
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function btoa(s) {
	s = String(s);
	let out = '';
	for (let i = 0; i < s.length; i += 3) {
		const a = s.charCodeAt(i), b = s.charCodeAt(i + 1),
			c = s.charCodeAt(i + 2);
		if (a > 255 || b > 255 || c > 255)
			throw new DOMException('not Latin1', 'InvalidCharacterError');
		const n = (a << 16) | ((b || 0) << 8) | (c || 0);
		out += B64[n >> 18 & 63] + B64[n >> 12 & 63] +
			(isNaN(b) ? '=' : B64[n >> 6 & 63]) +
			(isNaN(c) ? '=' : B64[n & 63]);
	}
	return out;
}
function atob(s) {
	s = String(s).replace(/[\t\n\f\r ]/g, '');
	if (s.length % 4 === 0)
		s = s.replace(/==?$/, '');
	if (s.length % 4 === 1 || /[^A-Za-z0-9+\/]/.test(s))
		throw new DOMException('bad base64', 'InvalidCharacterError');
	let out = '', bits = 0, n = 0;
	for (const ch of s) {
		n = (n << 6) | B64.indexOf(ch);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += String.fromCharCode((n >> bits) & 255);
		}
	}
	return out;
}

/* Blobs and form data, enough for the usual uses */
class Blob {
	constructor(parts = [], opts = {}) {
		this._s = parts.map((p) => p instanceof Blob ? p._s :
			typeof p === 'string' ? p : new TextDecoder().decode(p))
			.join('');
		this.type = opts.type || '';
	}
	get size() { return new TextEncoder().encode(this._s).length; }
	text() { return Promise.resolve(this._s); }
	arrayBuffer() {
		return Promise.resolve(new TextEncoder().encode(this._s).buffer);
	}
	slice(a, b, type) { return new Blob([this._s.slice(a, b)], { type }); }
}
class File extends Blob {
	constructor(parts, name, opts = {}) {
		super(parts, opts);
		this.name = String(name);
		this.lastModified = opts.lastModified || Date.now();
	}
}
class FormData {
	constructor(form, submitter) {
		this._list = [];
		if (!form)
			return;
		for (const e of form.elements) {
			const name = e.name;
			if (!name || e.disabled || e.closest('datalist'))
				continue;
			const t = e.type;
			if (/^(submit|image|button|reset)$/.test(t) && e !== submitter)
				continue;
			if (/^(checkbox|radio)$/.test(t) && !e.checked)
				continue;
			if (e.localName === 'select') {
				for (const o of e.options)
					if (o.selected)
						this._list.push([name, o.value]);
				continue;
			}
			if (t === 'file' || e.localName === 'fieldset' ||
			    e.localName === 'object' || e.localName === 'output')
				continue;
			this._list.push([name, e.value]);
		}
	}
	append(k, v) { this._list.push([String(k), v instanceof Blob ? v : String(v)]); }
	delete(k) { this._list = this._list.filter((x) => x[0] !== k); }
	get(k) {
		const x = this._list.find((y) => y[0] === k);
		return x ? x[1] : null;
	}
	getAll(k) { return this._list.filter((x) => x[0] === k).map((x) => x[1]); }
	has(k) { return this._list.some((x) => x[0] === k); }
	set(k, v) {
		this.delete(k);
		this.append(k, v);
	}
	forEach(fn, self) {
		for (const [k, v] of this._list)
			fn.call(self, v, k, this);
	}
	keys() { return this._list.map((x) => x[0])[Symbol.iterator](); }
	values() { return this._list.map((x) => x[1])[Symbol.iterator](); }
	entries() { return this._list.map((x) => x.slice())[Symbol.iterator](); }
	[Symbol.iterator]() { return this.entries(); }
}

/* Network: the host's request is blocking, so the answer is handed
 * over from a timer, after the caller's code has run. */

class Headers {
	constructor(init) {
		this._m = new Map();
		if (init instanceof Headers)
			init.forEach((v, k) => this.append(k, v));
		else if (Array.isArray(init))
			init.forEach(([k, v]) => this.append(k, v));
		else if (init)
			Object.keys(init).forEach((k) => this.append(k, init[k]));
	}
	append(k, v) {
		k = String(k).toLowerCase();
		v = String(v);
		this._m.set(k, this._m.has(k) ? this._m.get(k) + ', ' + v : v);
	}
	set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
	get(k) {
		const v = this._m.get(String(k).toLowerCase());
		return v === undefined ? null : v;
	}
	has(k) { return this._m.has(String(k).toLowerCase()); }
	delete(k) { this._m.delete(String(k).toLowerCase()); }
	forEach(fn, self) {
		for (const [k, v] of [...this._m].sort())
			fn.call(self, v, k, this);
	}
	getSetCookie() { return []; }
	keys() { return [...this._m.keys()].sort()[Symbol.iterator](); }
	values() {
		return [...this._m].sort().map((x) => x[1])[Symbol.iterator]();
	}
	entries() { return [...this._m].sort()[Symbol.iterator](); }
	[Symbol.iterator]() { return this.entries(); }
	_raw() {
		let s = '';
		for (const [k, v] of this._m)
			if (!/^(host|cookie|content-length|connection|accept-encoding)$/
			    .test(k))
				s += k + ': ' + v.replace(/[\r\n]/g, ' ') + '\r\n';
		return s;
	}
}

function parseHeaders(raw) {
	const h = new Headers();
	for (const line of raw.split(/\r?\n/)) {
		const i = line.indexOf(':');
		if (i > 0 && !/^set-cookie2?$/i.test(line.slice(0, i)))
			h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
	}
	return h;
}

function bodyString(b) {
	if (b === undefined || b === null)
		return null;
	if (b instanceof URLSearchParams)
		return String(b);
	if (b instanceof FormData) {
		const q = new URLSearchParams();
		for (const [k, v] of b)
			q.append(k, v instanceof Blob ? v._s : v);
		return String(q);
	}
	if (b instanceof Blob)
		return b._s;
	if (b instanceof ArrayBuffer || ArrayBuffer.isView(b))
		return new TextDecoder('latin1').decode(b);
	return String(b);
}

const STATUS_TEXT = { 200: 'OK', 201: 'Created', 204: 'No Content',
	301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified',
	400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden',
	404: 'Not Found', 500: 'Internal Server Error' };

class Response {
	constructor(body = null, init = {}) {
		this._body = body === null ? '' : bodyString(body);
		this.status = init.status === undefined ? 200 : init.status;
		this.statusText = init.statusText ||
			STATUS_TEXT[this.status] || '';
		this.headers = new Headers(init.headers);
		this.url = init.url || '';
		this.redirected = false;
		this.type = 'basic';
		this.bodyUsed = false;
	}
	get ok() { return this.status >= 200 && this.status < 300; }
	_take() {
		if (this.bodyUsed)
			return Promise.reject(new TypeError('body used'));
		this.bodyUsed = true;
		return Promise.resolve(this._body);
	}
	text() {
		return this._take().then((s) => {
			try {
				return decodeURIComponent(escape(s));
			} catch (e) {
				return s;
			}
		});
	}
	json() { return this.text().then(JSON.parse); }
	arrayBuffer() {
		return this._take().then((s) => {
			const a = new Uint8Array(s.length);
			for (let i = 0; i < s.length; i++)
				a[i] = s.charCodeAt(i) & 255;
			return a.buffer;
		});
	}
	blob() {
		return this.text().then((s) => new Blob([s],
			{ type: this.headers.get('content-type') || '' }));
	}
	formData() {
		return this.text().then((s) => {
			const f = new FormData();
			new URLSearchParams(s).forEach((v, k) => f.append(k, v));
			return f;
		});
	}
	clone() {
		const r = new Response(this._body, this);
		r.url = this.url;
		return r;
	}
	static json(v, init) {
		return new Response(JSON.stringify(v), init);
	}
	static error() { return new Response(null, { status: 0 }); }
	static redirect(url, status = 302) {
		return new Response(null, { status, headers: { location: url } });
	}
}

class Request {
	constructor(input, init = {}) {
		const r = input instanceof Request ? input : null;
		this.url = r ? r.url : new URL(String(input), document.baseURI).href;
		this.method = String(init.method || (r && r.method) || 'GET')
			.toUpperCase();
		this.headers = new Headers(init.headers || (r && r.headers));
		this._body = bodyString(init.body !== undefined ? init.body :
			r && r._body);
		this.credentials = init.credentials || 'same-origin';
		this.mode = init.mode || 'cors';
		this.signal = init.signal || null;
	}
	clone() { return new Request(this); }
}

/* The page's latin1 response body as UTF-8 text happens in text();
 * request() gives the bytes as a latin1 string. */
function doRequest(method, url, headers, body) {
	return api.request(method, url, headers, body);
}

function fetch(input, init = {}) {
	let req;
	try {
		req = new Request(input, init);
	} catch (e) {
		return Promise.reject(e);
	}
	return new Promise((resolve, reject) => {
		if (req.signal && req.signal.aborted) {
			reject(new DOMException('aborted', 'AbortError'));
			return;
		}
		addTimer(() => {
			if (req.signal && req.signal.aborted) {
				reject(new DOMException('aborted', 'AbortError'));
				return;
			}
			const h = req.headers;
			if (req._body !== null && !h.has('content-type'))
				h.set('content-type', 'text/plain;charset=UTF-8');
			const r = doRequest(req.method, req.url, h._raw(), req._body);
			if (!r) {
				reject(new TypeError('Failed to fetch'));
				return;
			}
			const res = new Response(r.body, { status: r.status,
				headers: parseHeaders(r.headers) });
			res.url = r.url;
			res.redirected = r.url !== req.url;
			resolve(res);
		}, 0, [], false);
	});
}

class XMLHttpRequest extends EventTarget {
	constructor() {
		super();
		this.readyState = 0;
		this.status = 0;
		this.statusText = '';
		this.responseText = '';
		this.responseXML = null;
		this.responseType = '';
		this.responseURL = '';
		this.timeout = 0;
		this.withCredentials = false;
		this.upload = new EventTarget();
		this._headers = new Headers();
		this._resHeaders = new Headers();
		this._aborted = false;
		for (const ev of ['readystatechange', 'load', 'error', 'abort',
			'loadend', 'loadstart', 'progress', 'timeout'])
			this['on' + ev] = null;
	}
	get response() {
		if (this.responseType === 'json') {
			try {
				return JSON.parse(this.responseText);
			} catch (e) {
				return null;
			}
		}
		if (this.responseType === 'arraybuffer') {
			const s = this._raw || '';
			const a = new Uint8Array(s.length);
			for (let i = 0; i < s.length; i++)
				a[i] = s.charCodeAt(i) & 255;
			return a.buffer;
		}
		if (this.responseType === 'blob')
			return new Blob([this.responseText]);
		return this.responseText;
	}
	open(method, url, async = true) {
		this._method = String(method).toUpperCase();
		this._url = new URL(String(url), document.baseURI).href;
		this._async = async !== false;
		this._headers = new Headers();
		this._aborted = false;
		this._state(1);
	}
	setRequestHeader(k, v) { this._headers.append(k, v); }
	overrideMimeType() {}
	getResponseHeader(k) { return this._resHeaders.get(k); }
	getAllResponseHeaders() {
		let s = '';
		this._resHeaders.forEach((v, k) => { s += k + ': ' + v + '\r\n'; });
		return s;
	}
	abort() {
		this._aborted = true;
		this.readyState = 0;
		this._fire('abort');
	}
	_state(n) {
		this.readyState = n;
		this._fire('readystatechange');
	}
	_fire(type) {
		const ev = type === 'readystatechange' ? new Event(type) :
			new ProgressEvent(type);
		const h = this['on' + type];
		ev.target = ev.currentTarget = this;
		if (typeof h === 'function')
			try {
				h.call(this, ev);
			} catch (e) {
				report(e);
			}
		const m = this[LISTENERS];
		if (m && m[type])
			for (const l of m[type].slice())
				try {
					l.fn.call(this, ev);
				} catch (e) {
					report(e);
				}
	}
	send(body) {
		const run = () => {
			if (this._aborted)
				return;
			const b = this._method === 'GET' || this._method === 'HEAD' ?
				null : bodyString(body);
			if (b !== null && !this._headers.has('content-type'))
				this._headers.set('content-type',
					'text/plain;charset=UTF-8');
			const r = doRequest(this._method, this._url,
				this._headers._raw(), b);
			if (this._aborted)
				return;
			if (!r) {
				this.status = 0;
				this._state(4);
				this._fire('error');
				this._fire('loadend');
				return;
			}
			this.status = r.status;
			this.statusText = STATUS_TEXT[r.status] || '';
			this.responseURL = r.url;
			this._resHeaders = parseHeaders(r.headers);
			this._raw = r.body;
			try {
				this.responseText = decodeURIComponent(escape(r.body));
			} catch (e) {
				this.responseText = r.body;
			}
			this._state(2);
			this._state(3);
			this._state(4);
			this._fire('load');
			this._fire('loadend');
		};
		this._fire('loadstart');
		if (this._async)
			addTimer(run, 0, [], false);
		else
			run();
	}
}
XMLHttpRequest.UNSENT = 0;
XMLHttpRequest.OPENED = 1;
XMLHttpRequest.HEADERS_RECEIVED = 2;
XMLHttpRequest.LOADING = 3;
XMLHttpRequest.DONE = 4;

class AbortSignal extends EventTarget {
	constructor() {
		super();
		this.aborted = false;
		this.reason = undefined;
		this.onabort = null;
	}
	throwIfAborted() {
		if (this.aborted)
			throw this.reason;
	}
	static abort(r) {
		const c = new AbortController();
		c.abort(r);
		return c.signal;
	}
	static timeout(ms) {
		const c = new AbortController();
		addTimer(() => c.abort(new DOMException('timeout',
			'TimeoutError')), ms, [], false);
		return c.signal;
	}
	static any(signals) {
		const c = new AbortController();
		for (const s of signals)
			s.addEventListener('abort', () => c.abort(s.reason));
		return c.signal;
	}
}
class AbortController {
	constructor() { this.signal = new AbortSignal(); }
	abort(reason) {
		const s = this.signal;
		if (s.aborted)
			return;
		s.aborted = true;
		s.reason = reason === undefined ?
			new DOMException('aborted', 'AbortError') : reason;
		const ev = new Event('abort');
		if (typeof s.onabort === 'function')
			s.onabort(ev);
		dispatch(s, ev);
	}
}

/* Storage, in memory for the page's life */
class Storage {
	constructor() {
		Object.defineProperty(this, '_m', { value: new Map() });
	}
	get length() { return this._m.size; }
	key(i) { return [...this._m.keys()][i] ?? null; }
	getItem(k) {
		const v = this._m.get(String(k));
		return v === undefined ? null : v;
	}
	setItem(k, v) { this._m.set(String(k), String(v)); }
	removeItem(k) { this._m.delete(String(k)); }
	clear() { this._m.clear(); }
}
const storageHandler = {
	get(t, p, r) {
		if (typeof p !== 'string' || p in t)
			return Reflect.get(t, p, r);
		return t.getItem(p) ?? undefined;
	},
	set(t, p, v) {
		t.setItem(p, v);
		return true;
	},
	deleteProperty(t, p) {
		t.removeItem(p);
		return true;
	},
};

/* Observers: no layout notifications; intersection observers say
 * "visible" once, which is what lazy loaders wait for. */
class MutationObserver {
	constructor(cb) { this._cb = cb; }
	observe() {}
	disconnect() {}
	takeRecords() { return []; }
}
class IntersectionObserver {
	constructor(cb, opts = {}) {
		this._cb = cb;
		this.root = opts.root || null;
		this.rootMargin = opts.rootMargin || '0px';
		this.thresholds = [].concat(opts.threshold || 0);
		this._targets = new Set();
	}
	observe(el) {
		if (this._targets.has(el))
			return;
		this._targets.add(el);
		addTimer(() => {
			if (!this._targets.has(el))
				return;
			const r = el.getBoundingClientRect();
			try {
				this._cb([{ target: el, isIntersecting: true,
					intersectionRatio: 1, boundingClientRect: r,
					intersectionRect: r,
					rootBounds: new DOMRect(0, 0, g.innerWidth,
						g.innerHeight),
					time: performance.now() }], this);
			} catch (e) {
				report(e);
			}
		}, 0, [], false);
	}
	unobserve(el) { this._targets.delete(el); }
	disconnect() { this._targets.clear(); }
	takeRecords() { return []; }
}
class ResizeObserver {
	constructor(cb) {
		this._cb = cb;
		this._targets = new Set();
	}
	observe(el) {
		this._targets.add(el);
		addTimer(() => {
			if (!this._targets.has(el))
				return;
			const r = el.getBoundingClientRect();
			const size = [{ inlineSize: r.width, blockSize: r.height }];
			try {
				this._cb([{ target: el, contentRect: r,
					borderBoxSize: size, contentBoxSize: size,
					devicePixelContentBoxSize: size }], this);
			} catch (e) {
				report(e);
			}
		}, 0, [], false);
	}
	unobserve(el) { this._targets.delete(el); }
	disconnect() { this._targets.clear(); }
}
class PerformanceObserver {
	observe() {}
	disconnect() {}
	takeRecords() { return []; }
}
PerformanceObserver.supportedEntryTypes = [];

/* The window */

class Window extends EventTarget {}
class Navigator {}
class Screen {}
class Location {}
class History {}
Object.setPrototypeOf(location, Location.prototype);
Object.setPrototypeOf(history, History.prototype);

const navigator = Object.create(Navigator.prototype, {
	userAgent: { value: 'Mozilla/5.0 (X11; Linux) Soie/0.1',
		enumerable: true },
	appName: { value: 'Netscape', enumerable: true },
	appCodeName: { value: 'Mozilla', enumerable: true },
	appVersion: { value: '5.0 (X11)', enumerable: true },
	product: { value: 'Gecko', enumerable: true },
	vendor: { value: '', enumerable: true },
	platform: { value: 'Linux', enumerable: true },
	language: { value: 'en-US', enumerable: true },
	languages: { value: Object.freeze(['en-US', 'en']), enumerable: true },
	cookieEnabled: { value: true, enumerable: true },
	onLine: { value: true, enumerable: true },
	doNotTrack: { value: '1', enumerable: true },
	hardwareConcurrency: { value: 1, enumerable: true },
	maxTouchPoints: { value: 0, enumerable: true },
	webdriver: { value: false, enumerable: true },
	pdfViewerEnabled: { value: false, enumerable: true },
	plugins: { value: list([]), enumerable: true },
	mimeTypes: { value: list([]), enumerable: true },
	javaEnabled: { value: () => false },
	sendBeacon: { value: () => true },
	vibrate: { value: () => false },
	registerProtocolHandler: { value: () => {} },
});
const screen = Object.create(Screen.prototype, {
	width: { get: () => api.viewport()[0], enumerable: true },
	height: { get: () => api.viewport()[1], enumerable: true },
	availWidth: { get: () => api.viewport()[0], enumerable: true },
	availHeight: { get: () => api.viewport()[1], enumerable: true },
	colorDepth: { value: 24, enumerable: true },
	pixelDepth: { value: 24, enumerable: true },
	orientation: { value: { type: 'landscape-primary', angle: 0,
		addEventListener() {} } },
});

const performance = {
	now: () => api.now() - start,
	timeOrigin: Date.now() - (api.now() - start),
	timing: { navigationStart: Date.now() },
	mark() {},
	measure() {},
	clearMarks() {},
	clearMeasures() {},
	getEntries: () => [],
	getEntriesByType: () => [],
	getEntriesByName: () => [],
	toJSON() { return {}; },
};

function fmt(args) {
	return args.map((a) => {
		if (typeof a === 'string')
			return a;
		if (a instanceof Error)
			return a + (a.stack ? '\n' + a.stack : '');
		if (a instanceof Node)
			return '[' + a.nodeName + ']';
		try {
			const s = JSON.stringify(a);
			return s === undefined ? String(a) : s;
		} catch (e) {
			return String(a);
		}
	}).join(' ');
}
const console = {};
for (const [k, level] of [['log', 0], ['info', 0], ['debug', 0],
	['trace', 0], ['dir', 0], ['table', 0], ['warn', 1], ['error', 2]])
	console[k] = (...a) => api.message(level, fmt(a));
console.assert = (c, ...a) => {
	if (!c)
		api.message(2, 'Assertion failed: ' + fmt(a));
};
for (const k of ['group', 'groupCollapsed', 'groupEnd', 'time', 'timeEnd',
	'timeLog', 'count', 'countReset', 'clear', 'profile', 'profileEnd',
	'timeStamp'])
	console[k] = () => {};

const crypto = {
	getRandomValues(a) {
		if (!ArrayBuffer.isView(a) || a instanceof Float32Array ||
		    a instanceof Float64Array)
			throw new DOMException('not an integer array',
				'TypeMismatchError');
		new Uint8Array(a.buffer, a.byteOffset, a.byteLength)
			.set(new Uint8Array(api.random(a.byteLength)));
		return a;
	},
	randomUUID() {
		const b = new Uint8Array(api.random(16));
		b[6] = (b[6] & 15) | 64;
		b[8] = (b[8] & 63) | 128;
		const h = [...b].map((x) => x.toString(16).padStart(2, '0'));
		return h.slice(0, 4).join('') + '-' + h.slice(4, 6).join('') +
			'-' + h.slice(6, 8).join('') + '-' +
			h.slice(8, 10).join('') + '-' + h.slice(10).join('');
	},
};

const CSS = {
	supports(a, b) {
		const t = b === undefined ? String(a) : a + ':' + b;
		return !/^\s*selector\(/.test(t);
	},
	escape: escapeIdent,
};

function getComputedStyle(el, pseudo) {
	check(el);
	return computedStyle(el, pseudo ? String(pseudo).replace(/^:+/, '') :
		null);
}

function matchMedia(q) {
	q = String(q);
	return Object.assign(new EventTarget(), {
		media: q,
		get matches() { return api.media(q); },
		onchange: null,
		addListener() {},
		removeListener() {},
	});
}


const selection = {
	rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null,
	focusNode: null, anchorOffset: 0, focusOffset: 0,
	removeAllRanges() {}, addRange() {}, getRangeAt() { return new Range(); },
	collapse() {}, collapseToEnd() {}, selectAllChildren() {},
	toString() { return ''; }, empty() {}, extend() {}, setBaseAndExtent() {},
};

/* Globals */
const globals = {
	window: g, self: g, top: g, parent: g, frames: g, opener: null,
	frameElement: null, closed: false, length: 0, name: '',
	status: '', origin: '', isSecureContext: true, crossOriginIsolated: false,
	location, history, navigator, screen, performance, console, CSS,
	crypto,
	customElements, NodeFilter,
	localStorage: new Proxy(new Storage(), storageHandler),
	sessionStorage: new Proxy(new Storage(), storageHandler),
	EventTarget, Event, CustomEvent, UIEvent, MouseEvent, PointerEvent,
	WheelEvent, KeyboardEvent, FocusEvent, InputEvent, SubmitEvent,
	ProgressEvent, ErrorEvent, PopStateEvent, HashChangeEvent,
	MessageEvent, Node, CharacterData, Text, CDATASection, Comment,
	DocumentType, DocumentFragment, Document, HTMLDocument: Document,
	Element, SVGElement, SVGSVGElement, MathMLElement, Attr,
	DOMTokenList, CSSStyleDeclaration, DOMRect, DOMRectReadOnly,
	DOMException, TreeWalker, NodeIterator: TreeWalker, Range,
	NodeList: Array, HTMLCollection: Array, URL, URLSearchParams,
	TextEncoder, TextDecoder, Blob, File, FormData, Headers, Request,
	Response, fetch, XMLHttpRequest, AbortController, AbortSignal,
	Storage, MutationObserver, IntersectionObserver, ResizeObserver,
	PerformanceObserver, Window, Navigator, Screen, Location, History,
	btoa, atob, getComputedStyle, matchMedia,
	devicePixelRatio: 1,
	setTimeout: (fn, d, ...a) => addTimer(fn, d, a, false),
	setInterval: (fn, d, ...a) => addTimer(fn, d, a, true),
	clearTimeout: (id) => { timers.delete(id); },
	clearInterval: (id) => { timers.delete(id); },
	setImmediate: (fn, ...a) => addTimer(fn, 0, a, false),
	clearImmediate: (id) => { timers.delete(id); },
	requestAnimationFrame: (fn) => addTimer(() => fn(performance.now()),
		16, [], false),
	cancelAnimationFrame: (id) => { timers.delete(id); },
	requestIdleCallback: (fn) => addTimer(() => fn({ didTimeout: false,
		timeRemaining: () => 10 }), 1, [], false),
	cancelIdleCallback: (id) => { timers.delete(id); },
	queueMicrotask: (fn) => { Promise.resolve().then(fn).catch(report); },
	structuredClone: (v) => v === undefined ? v :
		JSON.parse(JSON.stringify(v)),
	reportError: report,
	alert: (m) => api.message(3, str(m === undefined ? '' : m)),
	confirm: (m) => {
		api.message(3, str(m === undefined ? '' : m));
		return true;
	},
	prompt: () => null,
	print: () => {},
	focus: () => {},
	blur: () => {},
	stop: () => {},
	open: (url) => {
		if (url)
			navigate(String(url), false);
		return null;
	},
	close: () => {},
	postMessage: (data) => {
		addTimer(() => dispatch(g, new MessageEvent('message',
			{ data, origin: location.origin, source: g })), 0, [], false);
	},
	getSelection: () => selection,
	scrollTo: (x, y) => {
		if (typeof x === 'object' && x)
			api.scroll(x.left || 0, x.top || 0);
		else
			api.scroll(+x || 0, +y || 0);
	},
	scrollBy: (x, y) => {
		const v = api.viewport();
		if (typeof x === 'object' && x)
			api.scroll(v[2] + (x.left || 0), v[3] + (x.top || 0));
		else
			api.scroll(v[2] + (+x || 0), v[3] + (+y || 0));
	},
};
globals.scroll = globals.scrollTo;
globals.origin = (() => {
	try {
		return new URL(href).origin;
	} catch (e) {
		return 'null';
	}
})();
for (const k of Object.keys(globals))
	Object.defineProperty(g, k, { value: globals[k], writable: true,
		configurable: true, enumerable: false });
for (const [k, i] of [['innerWidth', 0], ['innerHeight', 1],
	['outerWidth', 0], ['outerHeight', 1], ['scrollX', 2], ['scrollY', 3],
	['pageXOffset', 2], ['pageYOffset', 3]])
	Object.defineProperty(g, k, { get: () => api.viewport()[i],
		set() {}, configurable: true });
Object.defineProperty(g, 'screenX', { value: 0, writable: true });
Object.defineProperty(g, 'screenY', { value: 0, writable: true });
Object.defineProperty(g, 'event', { value: undefined, writable: true });
Object.defineProperty(g, 'globalThis', { value: g, writable: true });
for (const ev of ['load', 'unload', 'beforeunload', 'resize', 'scroll',
	'error', 'message', 'popstate', 'hashchange', 'click', 'keydown',
	'keyup', 'focus', 'blur', 'online', 'offline', 'pageshow',
	'pagehide', 'storage', 'DOMContentLoaded', 'mousemove', 'mouseup',
	'mousedown', 'touchstart', 'touchend', 'wheel', 'orientationchange',
	'visibilitychange', 'unhandledrejection'])
	Object.defineProperty(g, 'on' + ev.toLowerCase(), {
		get() { return (g[HANDLERS] && g[HANDLERS][ev]) || null; },
		set(v) {
			(g[HANDLERS] || (g[HANDLERS] = Object.create(null)))[ev] =
				typeof v === 'function' ? v : null;
		},
		configurable: true,
	});
Object.setPrototypeOf(g, Window.prototype);

/* Constructors of elements */
g.Image = function Image(w, h) {
	const el = document.createElement('img');
	if (w !== undefined)
		el.width = w;
	if (h !== undefined)
		el.height = h;
	return el;
};
g.Image.prototype = HTMLImageElement.prototype;
g.Option = function Option(text = '', value, dflt, selected) {
	const el = document.createElement('option');
	el.text = text;
	if (value !== undefined)
		el.value = value;
	if (dflt)
		el.setAttribute('selected', '');
	if (selected)
		el.selected = true;
	return el;
};
g.Option.prototype = HTMLOptionElement.prototype;
g.Audio = function Audio(src) {
	const el = document.createElement('audio');
	if (src !== undefined)
		el.src = src;
	return el;
};
g.Audio.prototype = HTMLAudioElement.prototype;

/* The document object: the wrapper C made for the document node */
api.setDocument = function (d) {
	document = d;
	Object.defineProperty(g, 'document', { value: d, writable: false,
		configurable: true, enumerable: true });
	/* import maps: "imports" only, no scopes */
	for (const s of query(d, 'script[type=importmap]', 0)) {
		try {
			const m = JSON.parse(textOf(s)).imports || {};
			for (const k of Object.keys(m))
				api.mapModule(k, new URL(m[k], d.baseURI).href);
		} catch (e) {
			report(e);
		}
	}
};
})(__soie);
