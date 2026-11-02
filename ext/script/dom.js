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
/* A microtask on the engine's own job queue: a page may replace
 * Promise and its then (core-js's polyfill does, on the BBC, then
 * queues its jobs with queueMicrotask: ours would run on its own) */
const RESOLVED = Promise.resolve(), THEN = Promise.prototype.then;

function report(e) {
	let s;
	try {
		s = e && e.stack ? e + '\n' + e.stack : String(e);
	} catch (x) {
		s = 'error';
	}
	api.message(2, s);
	/* in a worker, an error event on its Worker too */
	if (api.worker)
		api.workerError(String(e));
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
		if (type === 'message' && this instanceof MessagePort)
			this.start();
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

/* MessageChannel: a message is a task of its own on the other port,
 * once that one is started (onmessage, addEventListener or start()) */
class MessagePort extends EventTarget {
	constructor() {
		super();
		this._other = null;
		this._started = false;
		this._queue = [];
		this._sched = false;
	}
	postMessage(data) {
		const to = this._other;
		if (!to)
			return;
		to._queue.push(data);
		to._flush();
	}
	_flush() {
		if (this._sched || !this._started || !this._queue.length)
			return;
		this._sched = true;
		addTimer(() => {
			this._sched = false;
			const data = this._queue.shift();
			this._flush();
			dispatch(this, new MessageEvent('message', { data }));
		}, 0, [], false);
	}
	start() {
		this._started = true;
		this._flush();
	}
	close() {
		if (this._other)
			this._other._other = null;
		this._other = null;
		this._queue = [];
	}
	get onmessage() {
		return (this[HANDLERS] && this[HANDLERS].message) || null;
	}
	set onmessage(fn) {
		(this[HANDLERS] || (this[HANDLERS] = Object.create(null)))
			.message = typeof fn === 'function' ? fn : null;
		this.start();
	}
}

class MessageChannel {
	constructor() {
		this.port1 = new MessagePort();
		this.port2 = new MessagePort();
		this.port1._other = this.port2;
		this.port2._other = this.port1;
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
		el = BODY_WINDOW.test(type) && document ? document.body : null;
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
		for (const el of query(document, 'link', 0))
			linkLoad(el, true);
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
					api.navigate(n.href, newWindow(n.target) ?
						2 : false);
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
};

/* ms until the next timer, -1 for none: asked after the microtasks,
 * which may set some (a fetch() answered sets the one of the next) */
api.nextTimer = function () {
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
	const nodes = api.type(node) === S_FRAG ? api.children(node, true) :
		[node];
	if (!api.insert(parent, node, ref || null))
		throw new DOMException('cannot insert there',
			'HierarchyRequestError');
	if (customs.size)
		connected(nodes);
	for (const n of nodes)
		linkLoad(n, false);
	return node;
}

/*
 * The load event of <link>. A style sheet's comes when the engine has
 * it (soie_link_sheet(), set by the style pass that fetched it, the
 * first look runs one), error when it failed, looked at every 50 ms
 * and said loaded after 30 s whatever happens; preloads are said
 * loaded from a task (the engine does not fetch them). Links of the
 * parsed page are looked at from the window's load. Loaders wait for it: YouTube's player inserts
 * the link of its CSS and waits for it before it takes a video, and
 * measures itself with that CSS.
 */
const linksLoaded = new WeakSet();
const linksWaiting = new Map();
let linksTimer = 0;
function linkLoad(el, now) {
	if (api.type(el) !== S_EL || api.name(el) !== 'link' ||
	    linksLoaded.has(el) || !el.isConnected || !el.getAttribute('href'))
		return;
	const rel = el.getAttribute('rel') || '';
	if (/(^|\s)stylesheet(\s|$)/i.test(rel)) {
		linksLoaded.add(el);
		linksWaiting.set(el, -(api.now() + 30000));
		if (now)
			linksCheck();
		if (linksWaiting.size && !linksTimer)
			linksTimer = addTimer(linksCheck, 50, [], true);
	} else if (/(^|\s)(preload|modulepreload)(\s|$)/i.test(rel)) {
		linksLoaded.add(el);
		if (now)
			dispatch(el, new Event('load'));
		else
			addTimer(() => dispatch(el, new Event('load')), 0, [],
				false);
	}
}
function linksCheck() {
	const t = api.now();
	for (const [el, until] of linksWaiting) {
		/* a negative deadline: not looked at yet */
		const r = api.linkSheet(el, until < 0);
		if (!r && t < Math.abs(until)) {
			linksWaiting.set(el, Math.abs(until));
			continue;
		}
		linksWaiting.delete(el);
		dispatch(el, new Event(r < 0 ? 'error' : 'load'));
	}
	if (!linksWaiting.size && linksTimer) {
		timers.delete(linksTimer);
		linksTimer = 0;
	}
}

/* Custom elements just inserted in the page: constructed if they were
 * not yet, connectedCallback */
function connected(nodes) {
	for (const n of nodes) {
		if (api.type(n) !== S_EL || !n.isConnected)
			continue;
		for (const el of [n, ...customsIn(n)]) {
			const c = customs.get(api.name(el));
			if (!c)
				continue;
			if (!built.has(el))
				upgrade(el, c);
			else
				callback(el, 'connectedCallback');
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
		return api.type(this) === S_DOC ? null : api.ownerDoc(this);
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
/* never made by the HTML parser: there for scripts that patch it */
class ProcessingInstruction extends CharacterData {
	get target() { return ''; }
}
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
		return api.byId(this, String(id));
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

/* Serialized as browsers give them back (CSSOM): a hex color becomes
 * rgb() or rgba() ("#252b3b": "rgb(37, 43, 59)"; Startpage compares
 * that string before showing its dark pages); url() and strings are
 * left alone */
const HEX_OR_SKIP = /url\([^)]*\)|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|#([0-9a-fA-F]+)\b/g;
function serializeValue(v) {
	return v.indexOf('#') < 0 ? v : v.replace(HEX_OR_SKIP, (m, h) => {
		if (!h || ![3, 4, 6, 8].includes(h.length))
			return m;
		if (h.length < 6)
			h = h.replace(/./g, '$&$&');
		const n = [0, 2, 4, 6].map((i) => parseInt(h.slice(i, i + 2), 16));
		if (h.length === 6 || n[3] === 255)
			return 'rgb(' + n.slice(0, 3).join(', ') + ')';
		/* the shortest alpha that gives the same byte back */
		let a = Math.round(n[3] / 255 * 100) / 100;
		if (Math.round(a * 255) !== n[3])
			a = Math.round(n[3] / 255 * 1000) / 1000;
		return 'rgba(' + n.slice(0, 3).join(', ') + ', ' + a + ')';
	});
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
	get cssText() {
		return this._get().map(([n, v, p]) => n + ': ' +
			(n.startsWith('--') ? v : serializeValue(v)) +
			(p ? ' !important' : '') + ';').join(' ');
	}
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
		if (!d.length)
			return '';
		return name.startsWith('--') ? d[d.length - 1][1] :
			serializeValue(d[d.length - 1][1]);
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
			insert(target, api.parse(this, v), null);
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
		insert(p, f, this);
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
	'loadedmetadata', 'canplay', 'volumechange', 'playing', 'waiting',
	'seeking', 'seeked', 'progress', 'durationchange', 'loadeddata',
	'loadstart', 'canplaythrough', 'emptied', 'ratechange', 'stalled',
	'suspend']) {
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
		api.focus(this);	/* the browser types into it */
		if (old)
			dispatch(old, new FocusEvent('blur'));
		dispatch(this, new FocusEvent('focus'));
		dispatch(this, new FocusEvent('focusin', { bubbles: true }));
	}
	blur() {
		if (activeElement !== this)
			return;
		activeElement = null;
		api.focus(null);
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
	set defaultChecked(v) { this.checked = v; }
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
const M_PLAY = 0, M_PAUSE = 1, M_SEEK = 3, M_VOLUME = 4, M_MUTE = 5,
	M_CLOSE = 6;
class TimeRanges {
	constructor(a = []) { this._a = a; }
	get length() { return this._a.length >> 1; }
	start(i) { return this._at(i, 0); }
	end(i) { return this._at(i, 1); }
	_at(i, k) {
		i = i >>> 0;
		if (i >= this.length)
			throw new DOMException('no range ' + i, 'IndexSizeError');
		return this._a[2 * i + k];
	}
}
html('HTMLMediaElement', [], (B) => class extends B {
	get _st() { return api.playerState(this); }
	get srcObject() { return this._srcObject || null; }
	set srcObject(v) {
		this._srcObject = v || null;
		mediaLoad(this);
	}
	get seeking() {
		const s = this._st;
		return this._seekMark !== undefined && !!s &&
			s.seeks === this._seekMark;
	}
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
		const s = api.playerState(this);
		if (s) {
			/* the player says when it is done: seeked */
			this._seekMark = s.seeks;
			api.player(this, M_SEEK, this._seekTo);
			this._seekTo = 0;
		}
		addTimer(() => {
			dispatch(this, new Event('seeking'));
			if (!s)
				dispatch(this, new Event('seeked'));
		}, 0, [], false);
	}
	get duration() {
		if (this._ms)
			return this._ms.duration;
		const s = this._st;
		return s && s.duration > 0 ? s.duration : NaN;
	}
	get readyState() {
		const s = this._st;
		if (this._ms) {
			/* what is buffered at the position */
			if (!s || !(s.duration > 0))
				return 0;
			const r = api.mseBuffered(this, -1), t = s.time;
			for (let i = 0; i < r.length; i += 2)
				if (t >= r[i] && t < r[i + 1])
					return r[i + 1] - t > 0.5 ||
						this._ms.readyState === 'ended' ? 4 : 2;
			return 1;
		}
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
	get defaultPlaybackRate() { return 1; }
	set defaultPlaybackRate(v) {}
	get error() {
		if (this._mseError)
			return { code: this._mseError, message: 'cannot play' };
		const s = this._st;
		return s && s.error ? { code: 4, message: 'cannot play' } : null;
	}
	get buffered() {
		if (this._ms)
			return new TimeRanges(api.mseBuffered(this, -1));
		const d = this.duration;
		return new TimeRanges(isNaN(d) ? [] : [0, d]);
	}
	get seekable() {
		const ms = this._ms;
		if (ms && ms._live)
			return new TimeRanges(ms._live.slice());
		if (ms && ms.duration === Infinity) {
			const b = this.buffered;
			return new TimeRanges(b.length ? [0, b.end(b.length - 1)] : []);
		}
		return this._ms ? new TimeRanges(isNaN(this.duration) ? [] :
			[0, this.duration]) : this.buffered;
	}
	get currentSrc() { return this._ms ? this.src : this.src; }
	canPlayType(t) { return api.canPlay(String(t)) ? 'maybe' : ''; }
	load() { mediaLoad(this, true); }
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
{
	const d = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype,
		'src');
	Object.defineProperty(HTMLMediaElement.prototype, 'src', {
		get: d.get,
		set(v) {
			d.set.call(this, v);
			mediaLoad(this);
		},
		configurable: true,
	});
}
reflect(HTMLMediaElement, ['preload', 'crossOrigin']);
reflect(HTMLMediaElement, ['autoplay', 'loop', 'controls',
	['defaultMuted', 'muted']], 'bool');
html('HTMLVideoElement', ['video'], (B) => class extends B {
	getVideoPlaybackQuality() {
		const s = this._st;
		return { creationTime: performance.now(),
			totalVideoFrames: s ? s.decoded : 0,
			droppedVideoFrames: s ? s.dropped : 0,
			corruptedVideoFrames: 0 };
	}
}, HTMLMediaElement);
reflect(HTMLVideoElement, ['poster'], 'url');
reflect(HTMLVideoElement, ['width', 'height'], 'int');
reflect(HTMLVideoElement, ['playsInline'], 'bool');
html('HTMLAudioElement', ['audio'], null, HTMLMediaElement);

/*
 * Media Source Extensions: a MediaSource attached to an element feeds
 * the element's player (api.mse*), each SourceBuffer appends to one of
 * its buffers. Appends are parsed at once: the events follow in a task.
 */
const S_REMOVE = 0, S_ABORT = 1, S_DROP = 2, S_DURATION = 3, S_END = 4;
function handlers(proto, events) {
	for (const ev of events)
		Object.defineProperty(proto, 'on' + ev, {
			get() { return (this[HANDLERS] && this[HANDLERS][ev]) || null; },
			set(v) {
				(this[HANDLERS] || (this[HANDLERS] = Object.create(null)))[ev] =
					typeof v === 'function' ? v : null;
			},
			configurable: true,
		});
}
function invalid(what) { return new DOMException(what, 'InvalidStateError'); }

/* The element's source: srcObject, else a src from createObjectURL();
 * a new one closes the player of the old one */
function mediaLoad(el, reload) {
	const src = el.getAttribute('src');
	const ms = el._srcObject instanceof MediaSource ? el._srcObject :
		!el._srcObject && src ? objectURLs.get(src) : null;
	if (el._ms && (el._ms !== ms || reload))
		el._ms._detach();
	else if (!el._ms && (src !== el._loaded || reload) &&
		 api.playerState(el)) {
		api.player(el, M_CLOSE, 0);
		addTimer(() => dispatch(el, new Event('emptied')), 0, [], false);
	}
	el._loaded = src;
	el._seekMark = undefined;
	if (ms instanceof MediaSource && !el._ms)
		ms._attach(el);
}

class SourceBufferList extends EventTarget {
	constructor() {
		super();
		this._l = [];
	}
	get length() { return this._l.length; }
	_set(l) {
		for (let i = 0; i < Math.max(l.length, this._l.length); i++)
			if (i < l.length)
				this[i] = l[i];
			else
				delete this[i];
		this._l = l;
	}
	[Symbol.iterator]() { return this._l[Symbol.iterator](); }
}
handlers(SourceBufferList.prototype, ['addsourcebuffer',
	'removesourcebuffer']);

class SourceBuffer extends EventTarget {
	constructor(ms, id, type) {
		super();
		this._ms = ms;
		this._id = id;
		this._type = type;
		this._offset = 0;
		this._ws = 0;
		this._we = Infinity;
		this._timer = 0;
		this.updating = false;
		this.mode = 'segments';
		this.audioTracks = list([]);
		this.videoTracks = list([]);
		this.textTracks = list([]);
	}
	_check() {
		if (!this._ms)
			throw invalid('removed from its MediaSource');
		if (this.updating)
			throw invalid('updating');
	}
	get timestampOffset() { return this._offset; }
	set timestampOffset(v) {
		this._check();
		this._offset = +v || 0;
	}
	get appendWindowStart() { return this._ws; }
	set appendWindowStart(v) {
		this._check();
		if (!(+v >= 0) || +v >= this._we)
			throw new TypeError('bad appendWindowStart');
		this._ws = +v;
	}
	get appendWindowEnd() { return this._we; }
	set appendWindowEnd(v) {
		this._check();
		if (isNaN(+v) || +v <= this._ws)
			throw new TypeError('bad appendWindowEnd');
		this._we = +v;
	}
	get buffered() {
		if (!this._ms)
			throw invalid('removed from its MediaSource');
		return new TimeRanges(this._ms._el ?
			api.mseBuffered(this._ms._el, this._id) : []);
	}
	_done(error) {
		this._timer = addTimer(() => {
			this._timer = 0;
			dispatch(this, new Event('updatestart'));
			this.updating = false;
			dispatch(this, new Event(error ? 'error' : 'update'));
			dispatch(this, new Event('updateend'));
			if (error && this._ms)
				this._ms.endOfStream('decode');
		}, 0, [], false);
	}
	appendBuffer(data) {
		const b = asBytes(data);
		if (!b)
			throw new TypeError('appendBuffer: not an ArrayBuffer');
		this._check();
		const ms = this._ms;
		if (ms._state === 'ended') {
			ms._state = 'open';
			api.mseControl(ms._el, S_END, -1, 0, 0);
			addTimer(() => dispatch(ms, new Event('sourceopen')), 0, [],
				false);
		}
		const r = api.mseAppend(ms._el, this._id, b, this._offset,
			this._ws, this._we);
		if (r === -2)
			throw new DOMException('the buffer is full',
				'QuotaExceededError');
		this.updating = true;
		this._done(r === -1);
		const el = ms._el;
		addTimer(() => dispatch(el, new Event('progress')), 0, [], false);
	}
	remove(start, end) {
		this._check();
		const ms = this._ms;
		if (isNaN(ms.duration) || !(+start >= 0) || +start > ms.duration ||
		    !(+end > +start))
			throw new TypeError('bad range');
		if (ms._state === 'ended')
			ms._state = 'open';
		api.mseControl(ms._el, S_REMOVE, this._id, +start,
			Math.min(+end, 1e300));
		this.updating = true;
		this._done(false);
	}
	abort() {
		if (!this._ms || this._ms._state !== 'open')
			throw invalid('not open');
		if (this.updating) {
			timers.delete(this._timer);
			this._timer = 0;
			this.updating = false;
			dispatch(this, new Event('abort'));
			dispatch(this, new Event('updateend'));
		}
		api.mseControl(this._ms._el, S_ABORT, this._id, 0, 0);
		this._ws = 0;
		this._we = Infinity;
	}
	changeType(type) {
		this._check();
		if (!MediaSource.isTypeSupported(type))
			throw new DOMException(type, 'NotSupportedError');
		this._type = String(type);
	}
}
handlers(SourceBuffer.prototype, ['updatestart', 'update', 'updateend',
	'error', 'abort']);

class MediaSource extends EventTarget {
	constructor() {
		super();
		this._state = 'closed';
		this._duration = NaN;
		this._el = null;
		this._live = null;
		this.sourceBuffers = new SourceBufferList();
		this.activeSourceBuffers = new SourceBufferList();
	}
	static isTypeSupported(type) { return api.mseSupported(String(type)); }
	get readyState() { return this._state; }
	get handle() { return this; }
	get duration() { return this._state === 'closed' ? NaN : this._duration; }
	set duration(v) {
		v = +v;
		if (isNaN(v) || v < 0)
			throw new TypeError('bad duration');
		if (this._state !== 'open' ||
		    this.sourceBuffers._l.some((b) => b.updating))
			throw invalid('cannot set the duration now');
		this._setDuration(v);
	}
	_setDuration(v) {
		if (v === this._duration)
			return;
		this._duration = v;
		api.mseControl(this._el, S_DURATION, -1, isFinite(v) ? v : 0, 0);
	}
	addSourceBuffer(type) {
		type = String(type);
		if (!type)
			throw new TypeError('no type');
		if (!MediaSource.isTypeSupported(type))
			throw new DOMException(type, 'NotSupportedError');
		if (this._state !== 'open')
			throw invalid('not open');
		const id = api.mseAdd(this._el, type);
		if (id < 0)
			throw new DOMException('too many SourceBuffers',
				'QuotaExceededError');
		const sb = new SourceBuffer(this, id, type);
		this.sourceBuffers._set([...this.sourceBuffers._l, sb]);
		this.activeSourceBuffers._set([...this.activeSourceBuffers._l, sb]);
		addTimer(() => {
			dispatch(this.sourceBuffers, new Event('addsourcebuffer'));
			dispatch(this.activeSourceBuffers, new Event('addsourcebuffer'));
		}, 0, [], false);
		return sb;
	}
	removeSourceBuffer(sb) {
		if (!this.sourceBuffers._l.includes(sb))
			throw new DOMException('not a SourceBuffer of this source',
				'NotFoundError');
		if (sb.updating) {
			timers.delete(sb._timer);
			sb.updating = false;
			dispatch(sb, new Event('abort'));
			dispatch(sb, new Event('updateend'));
		}
		if (this._el)
			api.mseControl(this._el, S_DROP, sb._id, 0, 0);
		sb._ms = null;
		this.sourceBuffers._set(this.sourceBuffers._l.filter((b) => b !== sb));
		this.activeSourceBuffers._set(this.activeSourceBuffers._l.filter(
			(b) => b !== sb));
		addTimer(() => {
			dispatch(this.sourceBuffers, new Event('removesourcebuffer'));
		}, 0, [], false);
	}
	endOfStream(error) {
		if (this._state !== 'open' ||
		    this.sourceBuffers._l.some((b) => b.updating))
			throw invalid('cannot end now');
		this._state = 'ended';
		addTimer(() => dispatch(this, new Event('sourceended')), 0, [],
			false);
		if (error) {
			const el = this._el;
			if (el) {
				el._mseError = error === 'network' ? 2 : 3;
				addTimer(() => dispatch(el, new Event('error')), 0, [],
					false);
			}
			return;
		}
		/* the duration: the end of what is buffered */
		let end = 0;
		for (const b of this.sourceBuffers._l) {
			const r = b.buffered;
			if (r.length)
				end = Math.max(end, r.end(r.length - 1));
		}
		if (end && (isNaN(this._duration) || end !== this._duration))
			this._setDuration(end);
		api.mseControl(this._el, S_END, -1, 1, 0);
	}
	setLiveSeekableRange(start, end) {
		if (this._state !== 'open')
			throw invalid('not open');
		if (!(+start >= 0) || +start > +end)
			throw new TypeError('bad range');
		this._live = [+start, +end];
	}
	clearLiveSeekableRange() {
		if (this._state !== 'open')
			throw invalid('not open');
		this._live = null;
	}
	_attach(el) {
		if (this._state !== 'closed' || !api.mseOpen(el)) {
			el._mseError = 4;
			addTimer(() => dispatch(el, new Event('error')), 0, [], false);
			return;
		}
		this._el = el;
		el._ms = this;
		el._mseError = 0;
		this._state = 'open';
		addTimer(() => {
			dispatch(el, new Event('loadstart'));
			dispatch(this, new Event('sourceopen'));
		}, 0, [], false);
	}
	_detach() {
		const el = this._el;
		for (const b of this.sourceBuffers._l) {
			timers.delete(b._timer);
			b.updating = false;
			b._ms = null;
		}
		this.sourceBuffers._set([]);
		this.activeSourceBuffers._set([]);
		this._state = 'closed';
		this._duration = NaN;
		this._el = null;
		this._live = null;
		if (el) {
			el._ms = null;
			api.player(el, M_CLOSE, 0);
			addTimer(() => dispatch(el, new Event('emptied')), 0, [],
				false);
		}
		addTimer(() => dispatch(this, new Event('sourceclose')), 0, [],
			false);
	}
}
handlers(MediaSource.prototype, ['sourceopen', 'sourceended',
	'sourceclose']);
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
/* The elements below n that may be custom: a selector of the defined
 * names (and customized built-ins), not a wrapper for every element */
let customSel = '';
function customsIn(n) {
	if (!customs.size)
		return [];
	if (!customSel)
		customSel = [...customs.keys()].map(escapeIdent).join(', ') +
			', [is]';
	return query(n, customSel, 0);
}
const observed = new Map();	/* name: the attributes it observes */
const built = new WeakSet();	/* custom elements constructed */
const NONE = new Set();
const customElements = {
	define(name, cls) {
		if (customs.has(name))
			throw new DOMException(name + ' already defined',
				'NotSupportedError');
		customs.set(name, cls);
		customSel = '';
		let o = [];
		try {
			o = cls.observedAttributes || [];
		} catch (e) {
			report(e);
		}
		observed.set(name, new Set(Array.from(o, String)));
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
	upgrade(root) { upgradeTree(root); },
};
const whenDefined = new Map();

function callback(el, name, ...args) {
	if (typeof el[name] === 'function')
		try {
			el[name](...args);
		} catch (e) {
			report(e);
		}
}

/* Construct a custom element in place: its constructor, its observed
 * attributes, connectedCallback when in the page */
function upgrade(el, cls) {
	if (built.has(el))
		return;
	built.add(el);
	if (Object.getPrototypeOf(el) !== cls.prototype)
		Object.setPrototypeOf(el, cls.prototype);
	const saved = constructing;
	constructing = el;
	try {
		/* the class's constructor body, on the existing element */
		Reflect.construct(cls, [], cls);
	} catch (e) {
		report(e);
		return;
	} finally {
		constructing = saved;
	}
	for (const a of observed.get(api.name(el)) || NONE) {
		const v = api.getAttr(el, a);
		if (v !== null && v !== undefined)
			callback(el, 'attributeChangedCallback', a, null, v, null);
	}
	if (el.isConnected)
		callback(el, 'connectedCallback');
}

/* The defined custom elements of a new subtree (innerHTML, a clone, an
 * import), unless it is a template's content or another document's:
 * those wait */
function upgradeTree(root) {
	if (!customs.size || !root)
		return;
	const t = api.type(root);
	if ((t !== S_EL && t !== S_FRAG && t !== S_DOC) ||
	    (t !== S_DOC && api.ownerDoc(root) !== document) ||
	    (t === S_DOC && root !== document))
		return;
	for (const el of t === S_EL ? [root, ...customsIn(root)] :
	    customsIn(root)) {
		const c = customs.get(api.name(el));
		if (c && !built.has(el))
			upgrade(el, c);
	}
}

function disconnected(n) {
	for (const el of [n, ...customsIn(n)])
		if (built.has(el))
			callback(el, 'disconnectedCallback');
}

/* Custom element reactions around the natives that change the tree:
 * every path of dom.js goes through them */
{
	const setAttr = api.setAttr, removeAttr = api.removeAttr;
	const remove = api.remove, clone = api.clone, parse = api.parse;
	const watched = (el, name) => customs.size && built.has(el) &&
		(observed.get(api.name(el)) || NONE).has(name);

	api.setAttr = function (el, name, v) {
		if (!watched(el, name))
			return setAttr(el, name, v);
		const old = api.getAttr(el, name);
		const r = setAttr(el, name, v);
		callback(el, 'attributeChangedCallback', name,
			old === undefined ? null : old, api.getAttr(el, name), null);
		return r;
	};
	api.removeAttr = function (el, name) {
		if (!watched(el, name))
			return removeAttr(el, name);
		const old = api.getAttr(el, name);
		const r = removeAttr(el, name);
		if (old !== null && old !== undefined)
			callback(el, 'attributeChangedCallback', name, old, null,
				null);
		return r;
	};
	api.remove = function (n) {
		const out = customs.size && api.type(n) === S_EL && n.isConnected;
		const r = remove(n);
		if (out)
			disconnected(n);
		return r;
	};
	api.clone = function (n, deep) {
		const c = clone(n, deep);
		upgradeTree(c);
		return c;
	};
	api.parse = function (ctx, html) {
		const f = parse(ctx, html);
		if (!ctx || api.name(ctx) !== 'template')
			upgradeTree(f);
		return f;
	};
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
	built.add(el);
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

/* document.all: falsy and typeof "undefined", but not undefined (HTML's
 * IsHTMLDDA); polymer-resin tells values from it */
const htmlAll = api.htmlAll();
Object.defineProperties(htmlAll, {
	length: { get: () => query(document, '*', 0).length },
	item: { value: (i) => htmlAll(i) },
	namedItem: { value: (name) => query(document, '[id="' + String(name)
		.replace(/["\\]/g, '\\$&') + '"], [name="' + String(name)
		.replace(/["\\]/g, '\\$&') + '"]', 1)[0] || null },
});

class DOMParser {
	parseFromString(s, type) {
		return api.newDocument(str(s));
	}
}

class XMLSerializer {
	serializeToString(n) { return api.serialize(check(n), true); }
}

let readyState = 'loading';
let activeElement = null;

/* document.head and .body: a child of the root <html> */
function htmlChild(doc, a, b) {
	const h = api.root(doc);
	if (!h || api.name(h) !== 'html')
		return null;
	for (const c of api.children(h, true)) {
		const n = api.name(c);
		if (n === a || n === b)
			return c;
	}
	return null;
}

function baseURL(doc, href) {
	const b = query(doc, 'base[href]', 1)[0];
	return b ? api.resolve(href, api.getAttr(b, 'href')) || href : href;
}

const baseOf = { gen: -1, href: '', url: '' };

class Document extends Node {
	get documentElement() { return api.root(this); }
	get head() {
		return htmlChild(this, 'head');
	}
	get body() {
		return htmlChild(this, 'body', 'frameset');
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
			t = api.createElement(NS_HTML, 'title', this);
			api.insert(h, t, null);
		}
		t.textContent = v;
	}
	get URL() { return location.href; }
	get documentURI() { return location.href; }
	get baseURI() {
		/* the page's is asked for each URL a script resolves: kept
		 * while neither its tree nor the address change */
		const g = api.gen(), h = location.href;
		if (this !== document)
			return baseURL(this, h);
		if (g !== baseOf.gen || h !== baseOf.href) {
			baseOf.url = baseURL(this, h);
			baseOf.gen = g;
			baseOf.href = h;
		}
		return baseOf.url;
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
	get defaultView() { return this === document ? g : null; }
	get all() { return htmlAll; }
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
			/* a document of its own, inert: no scripts, no
			 * rendering, nodes move to the page when inserted */
			createHTMLDocument(title) {
				const d = api.newDocument('<!DOCTYPE html><html>' +
					'<head></head><body></body></html>');
				if (title !== undefined)
					d.title = str(title);
				return d;
			},
			createDocument() {
				return api.newDocument('<html><head></head>' +
					'<body></body></html>');
			},
		};
	}
	hasFocus() { return true; }
	getElementById(id) {
		return api.byId(this, String(id));
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
		const el = api.createElement(NS_HTML, name.toLowerCase(), this);
		const is = opts && (typeof opts === 'string' ? opts : opts.is);
		const c = customs.get(name.toLowerCase()) || (is && customs.get(is));
		if (c && this === document)
			upgrade(el, c);
		return el;
	}
	createElementNS(ns, qname) {
		const i = NS_URI.indexOf(ns);
		const local = String(qname).replace(/^[^:]*:/, '');
		return api.createElement(i > 0 ? i : 0, local, this);
	}
	createTextNode(s) { return api.createText(str(s), this); }
	createComment(s) { return api.createComment(str(s), this); }
	createCDATASection(s) { return api.createText(str(s), this); }
	createDocumentFragment() { return api.createFragment(this); }
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
	importNode(n, deep) {
		const c = api.clone(check(n), !!deep);
		api.adopt(this, c);
		upgradeTree(c);
		return c;
	}
	adoptNode(n) {
		api.adopt(this, check(n));
		return n;
	}
	write(...s) { api.write(s.join('')); }
	writeln(...s) { api.write(s.join('') + '\n'); }
	open() { return this; }
	close() {}
	elementFromPoint(x, y) { return api.hit(+x || 0, +y || 0); }
	elementsFromPoint(x, y) {
		const r = [];
		for (let n = this.elementFromPoint(x, y); n && api.type(n) === S_EL;
		     n = api.parent(n))
			r.push(n);
		return r;
	}
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

/* Where relative URLs start: the document's base, or a worker's URL */
function startURL() {
	return document ? document.baseURI : href;
}

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

/* A browsing context name that is not this one: a new window */
function newWindow(target) {
	return !!target && !/^_(self|top|parent)$/i.test(target);
}

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

/* History: pushState changes the URL scripts see and the host shows (a
 * history entry of its own), with no load; to an address of the page's
 * origin only, as browsers (a SecurityError: the address bar would
 * lie, and the host would take the page for that URL's) */
function rewrite(url) {
	if (url === undefined || url === null)
		return href;
	const to = api.resolve(href, String(url));
	if (!to)
		throw new DOMException('Invalid URL', 'SyntaxError');
	if (!api.rewritable(href, to))
		throw new DOMException('A history state with the URL ' + to +
			' cannot be created in a document of ' + href,
			'SecurityError');
	return to;
}
const states = [{ state: null, url: href }];
let stateIndex = 0;
const history = {
	get length() { return states.length; },
	get state() { return states[stateIndex].state; },
	get scrollRestoration() { return 'auto'; },
	set scrollRestoration(v) {},
	pushState(state, title, url) {
		href = rewrite(url);
		states.splice(stateIndex + 1);
		states.push({ state, url: href });
		stateIndex++;
		api.showURL(href, true);
	},
	replaceState(state, title, url) {
		href = rewrite(url);
		states[stateIndex] = { state, url: href };
		api.showURL(href, false);
	},
	go(n) {
		n = n | 0;
		const to = stateIndex + n;
		if (!n || to < 0 || to >= states.length)
			return;
		stateIndex = to;
		href = states[to].url;
		api.showURL(href, false);
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

/* What createObjectURL() gave a URL to: Blobs, MediaSources */
const objectURLs = new Map();
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
	static createObjectURL(o) {
		const r = crypto.getRandomValues(new Uint8Array(16));
		const h = [...r].map((x) => (x + 256).toString(16).slice(1)).join('');
		const u = 'blob:' + location.origin + '/' + h.slice(0, 8) + '-' +
			h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) +
			'-' + h.slice(20);
		objectURLs.set(u, o);
		return u;
	}
	static revokeObjectURL(u) { objectURLs.delete(String(u)); }
}

/* Text encoding */
/* Bytes are Uint8Arrays; UTF-8 is native */
function asBytes(b) {
	return b instanceof Uint8Array ? b : b instanceof ArrayBuffer ?
		new Uint8Array(b) : ArrayBuffer.isView(b) ?
		new Uint8Array(b.buffer, b.byteOffset, b.byteLength) : null;
}
/* A latin1 string's bytes (data: URLs, atob()); UTF-8 beyond latin1 */
function latin1Bytes(s) {
	const a = new Uint8Array(s.length);
	for (let i = 0; i < s.length; i++) {
		const c = s.charCodeAt(i);
		if (c > 255)
			return new Uint8Array(api.utf8Encode(s));
		a[i] = c;
	}
	return a;
}
/* A copy of the bytes as an ArrayBuffer of their own */
function bufferOf(b) {
	return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}
class TextEncoder {
	get encoding() { return 'utf-8'; }
	encode(s = '') { return new Uint8Array(api.utf8Encode(String(s))); }
	encodeInto(s, dest) {
		const a = this.encode(s);
		const n = Math.min(a.length, dest.length);
		dest.set(a.subarray(0, n));
		return { read: s.length, written: n };
	}
}
class TextDecoder {
	constructor(label = 'utf-8', opts = {}) {
		const l = String(label).trim().toLowerCase();
		this.encoding = l === 'utf8' || l === 'unicode-1-1-utf-8' ?
			'utf-8' : l;
		this.fatal = !!opts.fatal;
		this.ignoreBOM = !!opts.ignoreBOM;
		this._rest = null;
	}
	decode(buf, opts = {}) {
		let a = buf ? asBytes(buf) : new Uint8Array(0);
		if (!a)
			throw new TypeError('decode: not bytes');
		if (this.encoding !== 'utf-8') {
			let s = '';
			for (let i = 0; i < a.length; i += 8192)
				s += String.fromCharCode.apply(null,
					a.subarray(i, i + 8192));
			return s;
		}
		if (this._rest) {
			const j = new Uint8Array(this._rest.length + a.length);
			j.set(this._rest);
			j.set(a, this._rest.length);
			a = j;
			this._rest = null;
		}
		if (opts.stream) {
			/* a sequence cut at the end waits for the next bytes */
			let i = a.length - 1, k = 0;
			while (i >= 0 && k < 3 && (a[i] & 0xc0) === 0x80) {
				i--;
				k++;
			}
			if (i >= 0 && a[i] >= 0xc0) {
				const need = a[i] >= 0xf0 ? 4 : a[i] >= 0xe0 ? 3 : 2;
				if (a.length - i < need) {
					this._rest = a.slice(i);
					a = a.subarray(0, i);
				}
			}
		}
		return api.utf8Decode(a);
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
		const l = [...parts].map((p) => p instanceof Blob ? p._b :
			asBytes(p) || new TextEncoder().encode(p));
		this._b = new Uint8Array(l.reduce((n, b) => n + b.length, 0));
		let n = 0;
		for (const b of l) {
			this._b.set(b, n);
			n += b.length;
		}
		this.type = opts.type ? String(opts.type).toLowerCase() : '';
	}
	get size() { return this._b.length; }
	text() { return Promise.resolve(api.utf8Decode(this._b)); }
	arrayBuffer() { return Promise.resolve(bufferOf(this._b)); }
	bytes() { return Promise.resolve(this._b.slice()); }
	slice(a, b, type) { return new Blob([this._b.slice(a, b)], { type }); }
	stream() { return new Response(this._b).body; }
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
			if (TOKEN.test(k) && !FORBIDDEN_HEADER.test(k))
				s += k + ': ' + v.replace(/[\0-\x1f\x7f]/g, ' ') +
					'\r\n';
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

/* A body: null, a string (sent as UTF-8) or bytes */
function bodyString(b) {
	if (b === undefined || b === null)
		return null;
	if (b instanceof URLSearchParams || typeof b === 'string')
		return String(b);
	if (b instanceof FormData)
		return multipart(b);
	if (b instanceof Blob)
		return b._b;
	return asBytes(b) || String(b);
}

/* The content type a body brings, null for none (bytes) */
function bodyType(b) {
	if (b instanceof URLSearchParams)
		return 'application/x-www-form-urlencoded;charset=UTF-8';
	if (b instanceof FormData)
		return 'multipart/form-data; boundary=' + boundary(b);
	if (b instanceof Blob)
		return b.type || null;
	if (b === undefined || b === null || asBytes(b))
		return null;
	return 'text/plain;charset=UTF-8';
}

/* Form data as browsers send it: multipart/form-data (servers read
 * their form fields from it, not from a text/plain body) */
function boundary(fd) {
	if (!fd._boundary)
		fd._boundary = '----SoieFormBoundary' + [...new Uint8Array(
			api.random(12))].map((x) => x.toString(16)
			.padStart(2, '0')).join('');
	return fd._boundary;
}
function multipart(fd) {
	const enc = new TextEncoder(), parts = [], b = boundary(fd);
	const esc = (s) => s.replace(/"/g, '%22').replace(/\r/g, '%0D')
		.replace(/\n/g, '%0A');
	for (const [k, v] of fd._list) {
		let h = '--' + b + '\r\nContent-Disposition: form-data; name="' +
			esc(k) + '"';
		if (v instanceof Blob) {
			h += '; filename="' + esc(v instanceof File ? v.name : 'blob') +
				'"\r\nContent-Type: ' + (v.type || 'application/octet-stream');
			parts.push(enc.encode(h + '\r\n\r\n'), v._b,
				enc.encode('\r\n'));
		} else {
			parts.push(enc.encode(h + '\r\n\r\n' +
				v.replace(/\r\n|\r|\n/g, '\r\n') + '\r\n'));
		}
	}
	parts.push(enc.encode('--' + b + '--\r\n'));
	return new Blob(parts)._b;
}
function bodyText(b) { return typeof b === 'string' ? b : api.utf8Decode(b); }
function bodyBytes(b) {
	return typeof b === 'string' ? new Uint8Array(api.utf8Encode(b)) : b;
}

/* Streams, enough to read a body (fetch's response.body) or a source
 * the page writes to: chunks queue up, read() takes them in order. */
class ReadableStream {
	constructor(src = {}) {
		this._q = [];
		this._wait = [];
		this._done = false;
		this._error = null;
		this.locked = false;
		const c = {
			enqueue: (v) => {
				const w = this._wait.shift();
				if (w)
					w({ value: v, done: false });
				else
					this._q.push(v);
			},
			close: () => {
				this._done = true;
				for (const w of this._wait.splice(0))
					w({ value: undefined, done: true });
			},
			error: (e) => {
				this._error = e || new TypeError('stream error');
				this._done = true;
				for (const w of this._wait.splice(0))
					w(Promise.reject(this._error));
			},
			get desiredSize() { return 1; },
		};
		this._pull = src.pull ? () => src.pull(c) : null;
		this._cancel = src.cancel;
		try {
			if (src.start)
				src.start(c);
		} catch (e) {
			c.error(e);
		}
	}
	_read() {
		if (this._q.length)
			return Promise.resolve({ value: this._q.shift(), done: false });
		if (this._error)
			return Promise.reject(this._error);
		if (this._done)
			return Promise.resolve({ value: undefined, done: true });
		const p = new Promise((r) => this._wait.push(r));
		if (this._pull)
			Promise.resolve().then(this._pull).catch(() => {});
		return p;
	}
	getReader() {
		if (this.locked)
			throw new TypeError('stream locked');
		this.locked = true;
		const s = this;
		return {
			read: () => s._read(),
			releaseLock() { s.locked = false; },
			cancel(r) { return s.cancel(r); },
			get closed() { return Promise.resolve(); },
		};
	}
	cancel(r) {
		this._q = [];
		this._done = true;
		if (this._cancel)
			try {
				this._cancel(r);
			} catch (e) {
				report(e);
			}
		return Promise.resolve();
	}
	async *[Symbol.asyncIterator]() {
		const r = this.getReader();
		for (;;) {
			const { value, done } = await r.read();
			if (done)
				return;
			yield value;
		}
	}
}

const STATUS_TEXT = { 200: 'OK', 201: 'Created', 204: 'No Content',
	301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified',
	400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden',
	404: 'Not Found', 500: 'Internal Server Error' };

class Response {
	constructor(body = null, init = {}) {
		this._body = body === null ? '' : bodyString(body);
		this._stream = null;
		this.status = init.status === undefined ? 200 : init.status;
		this.statusText = init.statusText ||
			STATUS_TEXT[this.status] || '';
		this.headers = new Headers(init.headers);
		const type = bodyType(body);
		if (type && !this.headers.has('content-type'))
			this.headers.set('content-type', type);
		this.url = init.url || '';
		this.redirected = false;
		this.type = 'basic';
		this.bodyUsed = false;
	}
	get ok() { return this.status >= 200 && this.status < 300; }
	/* the whole body as one chunk */
	get body() {
		if (!this._stream) {
			const b = this._body;
			this._stream = new ReadableStream({ start: (c) => {
				this.bodyUsed = true;
				if (b.length)
					c.enqueue(bodyBytes(b));
				c.close();
			} });
		}
		return this._stream;
	}
	_take() {
		if (this.bodyUsed)
			return Promise.reject(new TypeError('body used'));
		this.bodyUsed = true;
		return Promise.resolve(this._body);
	}
	text() { return this._take().then(bodyText); }
	json() { return this.text().then(JSON.parse); }
	arrayBuffer() {
		return this._take().then((b) => bufferOf(bodyBytes(b)));
	}
	bytes() { return this._take().then((b) => bodyBytes(b).slice()); }
	blob() {
		return this._take().then((b) => new Blob([bodyBytes(b)],
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

/* A request's state is its own: fetch() reads it, not the properties a
 * page may redefine (YouTube makes a Request of a data: URL holding the
 * player's answer, then gives it another url and method: browsers
 * fetch the data: URL) */
const REQ = Symbol('request');
class Request {
	constructor(input, init = {}) {
		const r = input instanceof Request ? input[REQ] : null;
		const body = init.body !== undefined ? init.body : r && r.body;
		this[REQ] = {
			url: r ? r.url : new URL(String(input), startURL()).href,
			method: method(init.method || (r && r.method) || 'GET'),
			headers: new Headers(init.headers || (r && r.headers)),
			body: bodyString(body),
			credentials: init.credentials || (r && r.credentials) ||
				'same-origin',
			mode: init.mode || (r && r.mode) || 'cors',
			signal: init.signal || (r && r.signal) || null,
		};
		const type = init.body !== undefined ? bodyType(body) : null;
		if (type && !this[REQ].headers.has('content-type'))
			this[REQ].headers.set('content-type', type);
	}
	get url() { return this[REQ].url; }
	get method() { return this[REQ].method; }
	get headers() { return this[REQ].headers; }
	get credentials() { return this[REQ].credentials; }
	get mode() { return this[REQ].mode; }
	get signal() { return this[REQ].signal; }
	get _body() { return this[REQ].body; }
	text() {
		const b = this[REQ].body;
		return Promise.resolve(b === null ? '' : bodyText(b));
	}
	arrayBuffer() {
		const b = this[REQ].body;
		return Promise.resolve(bufferOf(bodyBytes(b === null ? '' : b)));
	}
	json() { return this.text().then(JSON.parse); }
	clone() { return new Request(this); }
}

/* A data: or blob: URL's bytes and type, or null */
function localURL(url) {
	if (!/^blob:/i.test(url))
		return dataURL(url);
	const b = objectURLs.get(url.replace(/#.*/, ''));
	return b instanceof Blob ? { body: b._b, type: b.type ||
		'application/octet-stream' } : null;
}
/* A data: URL's bytes and type, or null */
function dataURL(url) {
	const m = /^data:([^,]*?)(;base64)?,(.*)$/is.exec(url);
	if (!m)
		return null;
	let body;
	try {
		body = latin1Bytes(m[2] ? atob(m[3].replace(/\s+/g, '')) :
			unescape(m[3]));
	} catch (e) {
		return null;
	}
	return { body, type: m[1] || 'text/plain;charset=US-ASCII' };
}

/* request() gives a response's body as an ArrayBuffer, text() decodes
 * it. */
/*
 * Requests from scripts, as browsers make them: an Origin header off
 * the page's origin, a preflight for what a form could not send, and
 * the answer readable only when the other origin allows it (CORS).
 * credentials: 'omit', 'same-origin' (the default) or 'include'.
 * Returns the response, null for a network error.
 */
const TOKEN = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
const FORBIDDEN_HEADER = /^(accept-charset|accept-encoding|access-control-request-headers|access-control-request-method|connection|content-length|cookie|cookie2|date|dnt|expect|host|keep-alive|origin|referer|set-cookie|te|trailer|transfer-encoding|upgrade|via|proxy-.*|sec-.*)$/i;
const SIMPLE_HEADER = /^(accept|accept-language|content-language|content-type)$/i;

function method(m) {
	m = String(m);
	if (!TOKEN.test(m))
		throw new TypeError("'" + m + "' is not a valid method");
	const u = m.toUpperCase();
	if (/^(CONNECT|TRACE|TRACK)$/.test(u))
		throw new TypeError("'" + m + "' is a forbidden method");
	return /^(DELETE|GET|HEAD|OPTIONS|POST|PUT|PATCH)$/.test(u) ? u : m;
}

function sameOrigin(url) {
	try {
		return new URL(url).origin === location.origin;
	} catch (e) {
		return false;
	}
}

function corsAllows(r, credentials) {
	const h = parseHeaders(r.headers);
	const o = h.get('access-control-allow-origin');
	if (credentials)
		return o === location.origin &&
			h.get('access-control-allow-credentials') === 'true';
	return o === '*' || o === location.origin;
}

function simpleRequest(m, headers) {
	if (!/^(GET|HEAD|POST)$/.test(m))
		return false;
	for (const [k, v] of headers) {
		if (!SIMPLE_HEADER.test(k))
			return false;
		if (/^content-type$/i.test(k) && !/^(application\/x-www-form-urlencoded|multipart\/form-data|text\/plain)\s*(;|$)/i.test(v))
			return false;
	}
	return true;
}

/* A request's steps (a CORS preflight, the request): each yield is the
 * arguments of a request, answered by the driver, synchronous
 * (doRequest) or in the background (doRequestAsync) */
function* requestSteps(m, url, headers, body, credentials = 'same-origin',
	mode = 'cors') {
	const same = sameOrigin(url);
	const send = credentials === 'include' ||
		(credentials === 'same-origin' && same);
	const NO_COOKIES = 1;
	let raw = headers._raw();
	if (!same || !/^(GET|HEAD)$/.test(m))
		raw += 'origin: ' + location.origin + '\r\n';
	if (!same && mode !== 'no-cors' && !simpleRequest(m, headers)) {
		const names = [...headers.keys()].filter((k) =>
			!SIMPLE_HEADER.test(k)).join(',');
		const p = yield ['OPTIONS', url, 'origin: ' +
			location.origin + '\r\naccess-control-request-method: ' + m +
			'\r\n' + (names ? 'access-control-request-headers: ' +
			names + '\r\n' : ''), null, NO_COOKIES];
		if (!p || p.status < 200 || p.status > 299 ||
		    !corsAllows(p, send))
			return null;
		const ph = parseHeaders(p.headers);
		const allowed = (ph.get('access-control-allow-methods') || '')
			.toUpperCase().split(/\s*,\s*/);
		if (!/^(GET|HEAD|POST)$/.test(m) && !allowed.includes(m) &&
		    !allowed.includes('*'))
			return null;
	}
	const r = yield [m, url, raw, body, send ? 0 : NO_COOKIES];
	if (!r)
		return null;
	if (!same && mode === 'no-cors')
		return { status: 0, url: '', headers: '', body: '', opaque: true };
	/* a redirection to another origin needs its consent too */
	if ((!same || !sameOrigin(r.url)) && !corsAllows(r, send))
		return null;
	return r;
}

function doRequest(...a) {
	const it = requestSteps(...a);
	let s = it.next();
	while (!s.done)
		s = it.next(api.request(...s.value));
	return s.value;
}

/* Requests out in the background, by id: what to call with the answer */
const out = new Map();
api.received = function (id, r) {
	const f = out.get(id);
	out.delete(id);
	if (f)
		f(r);
};

async function doRequestAsync(...a) {
	const it = requestSteps(...a);
	let s = it.next();
	while (!s.done) {
		const args = s.value;
		s = it.next(await new Promise((resolve) => {
			out.set(api.requestAsync(...args), resolve);
		}));
	}
	return s.value;
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
			if (/^(data|blob):/i.test(req.url)) {
				/* no network: the URL holds the bytes */
				const d = localURL(req.url);
				if (d)
					answered({ status: 200, url: req.url, body: d.body,
						headers: 'content-type: ' + d.type + '\r\n' });
				else
					reject(new TypeError('Failed to fetch'));
				return;
			}
			const h = req.headers;
			doRequestAsync(req.method, req.url, h, req._body,
				req.credentials, req.mode).then(answered, () =>
				reject(new TypeError('Failed to fetch')));
		}, 0, [], false);
		function answered(r) {
			if (req.signal && req.signal.aborted) {
				reject(new DOMException('aborted', 'AbortError'));
				return;
			}
			if (!r) {
				reject(new TypeError('Failed to fetch'));
				return;
			}
			if (r.opaque) {
				const o = new Response(null, { status: 200 });
				o.status = 0;
				o.type = 'opaque';
				resolve(o);
				return;
			}
			const res = new Response(r.body === '' ? null : r.body, {
				status: r.status,
				headers: parseHeaders(r.headers) });
			res.url = r.url;
			res.redirected = r.url !== req.url;
			resolve(res);
		}
	});
}

class XMLHttpRequest extends EventTarget {
	constructor() {
		super();
		this.readyState = 0;
		this.status = 0;
		this.statusText = '';
		this._raw = null;
		this._text = null;
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
		if (this.responseType === 'arraybuffer')
			return this._raw ? bufferOf(this._raw) : null;
		if (this.responseType === 'blob')
			return new Blob(this._raw ? [this._raw] : []);
		return this.responseText;
	}
	get responseText() {
		if (this._text === null)
			this._text = this._raw ? api.utf8Decode(this._raw) : '';
		return this._text;
	}
	open(m, url, async = true) {
		this._method = method(m);
		this._url = new URL(String(url), startURL()).href;
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
			const type = b !== null && bodyType(body);
			if (type && !this._headers.has('content-type'))
				this._headers.set('content-type', type);
			const args = [this._method, this._url, this._headers, b,
				this.withCredentials ? 'include' : 'same-origin'];
			if (/^(data|blob):/i.test(this._url)) {
				const d = localURL(this._url);
				done(d && { status: 200, url: this._url, body: d.body,
					headers: 'content-type: ' + d.type + '\r\n' });
				return;
			}
			/* asynchronous ones in the background: the page lives
			 * on while they wait */
			if (this._async)
				doRequestAsync(...args).then(done, () => done(null));
			else
				done(doRequest(...args));
		};
		const done = (r) => {
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
			this._raw = asBytes(r.body) || new Uint8Array(0);
			this._text = null;
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
/*
 * MutationObserver: records made around the natives that change the
 * tree (insert, remove, attributes, text), delivered in a microtask.
 * Nothing is looked at while no observer observes.
 */
class MutationRecord {
	constructor(type, target, o = {}) {
		this.type = type;
		this.target = target;
		this.addedNodes = list(o.added || []);
		this.removedNodes = list(o.removed || []);
		this.previousSibling = o.prev || null;
		this.nextSibling = o.next || null;
		this.attributeName = o.name === undefined ? null : o.name;
		this.attributeNamespace = null;
		this.oldValue = o.old === undefined ? null : o.old;
	}
}

/* A node holds its registrations ([{ observer, options }]), an observer
 * its nodes weakly: a tree dropped goes with its observers. Kept the
 * other way (a WeakMap of nodes, a set of them in the observer, the
 * value holding the key), no tree a MutationObserver watched was ever
 * freed: YouTube's components, one copy a video. */
const MO_REGS = Symbol('registrations');
let nobserving = 0;			/* registrations, all nodes */
const pendingObservers = new Set();

function observedMutation(type, target, o) {
	const seen = new Set();
	for (let n = target; n; n = api.parent(n)) {
		const regs = n[MO_REGS];
		if (!regs)
			continue;
		for (const { observer, options: op } of regs) {
			if (seen.has(observer) || (n !== target && !op.subtree))
				continue;
			if (type === 'childList' ? !op.childList :
			    type === 'attributes' ? !op.attributes ||
			    (op.attributeFilter && !op.attributeFilter.includes(
				o.name)) : !op.characterData)
				continue;
			seen.add(observer);
			const keep = type === 'attributes' ? op.attributeOldValue :
				type === 'characterData' ? op.characterDataOldValue :
				false;
			observer._records.push(new MutationRecord(type, target,
				keep ? o : Object.assign({}, o, { old: undefined })));
			if (!pendingObservers.size)
				queueMicrotask(notifyObservers);
			pendingObservers.add(observer);
		}
	}
}

function notifyObservers() {
	const all = [...pendingObservers];
	pendingObservers.clear();
	for (const ob of all) {
		const r = ob.takeRecords();
		if (r.length)
			try {
				ob._cb.call(ob, r, ob);
			} catch (e) {
				report(e);
			}
	}
}

class MutationObserver {
	constructor(cb) {
		if (typeof cb !== 'function')
			throw new TypeError('MutationObserver: not a function');
		this._cb = cb;
		this._records = [];
		this._nodes = [];	/* WeakRefs */
	}
	observe(target, options = {}) {
		check(target);
		const op = Object.assign({}, options);
		if (op.attributeOldValue || op.attributeFilter)
			op.attributes = op.attributes !== false;
		if (op.characterDataOldValue)
			op.characterData = op.characterData !== false;
		if (!op.childList && !op.attributes && !op.characterData)
			throw new TypeError('MutationObserver: nothing to observe');
		if (op.attributeFilter)
			op.attributeFilter = Array.from(op.attributeFilter, String);
		let regs = target[MO_REGS];
		if (!regs)
			target[MO_REGS] = regs = [];
		const r = regs.find((x) => x.observer === this);
		if (r) {
			r.options = op;
		} else {
			regs.push({ observer: this, options: op });
			nobserving++;
			this._nodes = this._nodes.filter((w) => w.deref());
			this._nodes.push(new WeakRef(target));
		}
	}
	disconnect() {
		for (const w of this._nodes) {
			const n = w.deref();
			const regs = n && n[MO_REGS] || [];
			const i = regs.findIndex((x) => x.observer === this);
			if (i >= 0) {
				regs.splice(i, 1);
				nobserving--;
			}
		}
		this._nodes = [];
		this._records = [];
	}
	takeRecords() {
		const r = this._records;
		this._records = [];
		return r;
	}
}

{
	const setAttr = api.setAttr, removeAttr = api.removeAttr;
	const setData = api.setData, insertNode = api.insert;
	const remove = api.remove;

	api.setAttr = function (el, name, v) {
		if (nobserving)
			observedMutation('attributes', el, { name,
				old: api.getAttr(el, name) });
		return setAttr(el, name, v);
	};
	api.removeAttr = function (el, name) {
		if (nobserving && api.getAttr(el, name) !== null)
			observedMutation('attributes', el, { name,
				old: api.getAttr(el, name) });
		return removeAttr(el, name);
	};
	api.setData = function (n, v) {
		if (nobserving)
			observedMutation('characterData', n, { old: api.data(n) });
		return setData(n, v);
	};
	api.remove = function (n) {
		const p = nobserving && api.parent(n);
		if (p)
			observedMutation('childList', p, { removed: [n],
				prev: api.prev(n), next: api.next(n) });
		return remove(n);
	};
	api.insert = function (parent, node, ref) {
		if (!nobserving)
			return insertNode(parent, node, ref);
		const frag = api.type(node) === S_FRAG;
		const added = frag ? api.children(node, false) : [node];
		const old = !frag && api.parent(node);
		if (old)
			observedMutation('childList', old, { removed: [node],
				prev: api.prev(node), next: api.next(node) });
		const prev = ref ? api.prev(ref) : api.last(parent);
		const r = insertNode(parent, node, ref);
		if (r && added.length)
			observedMutation('childList', parent, { added, prev,
				next: ref || null });
		return r;
	};
}
/*
 * IntersectionObserver: targets against the viewport (or a root), grown
 * by rootMargin; an entry at observe() and whenever a target crosses a
 * threshold, in the rendering step (api.frame()).
 */
const intersectionObservers = new Set();

/*
 * Observers are held by the page while one of their targets is in the
 * document. With all their targets out, only by those targets (a node
 * holds its observers) and by scripts: they rest, a weak reference
 * here, their targets weak in them, and wake when a target is back.
 * Held for good, they kept every component YouTube dropped, their
 * callbacks closing over it.
 */
const OBSERVERS = Symbol('observers');
const resting = new Set();	/* WeakRefs of observers whose targets are out */

function observedBy(el, o, on) {
	let s = el[OBSERVERS];
	if (on) {
		if (!s)
			el[OBSERVERS] = s = new Set();
		s.add(o);
	} else if (s) {
		s.delete(o);
	}
}

/* After a check: targets out of the document, already told so, rest
 * weakly; the observer too when none is left in */
function rest(o, active) {
	for (const [el, last] of o._targets)
		if (!el.isConnected && last !== undefined && o._out(last)) {
			o._targets.delete(el);
			o._away.push([new WeakRef(el), last]);
		}
	if (!o._targets.size && active.has(o)) {
		active.delete(o);
		if (o._away.length)
			resting.add(new WeakRef(o));
	}
}

/* Resting observers with a target back in the document: active again */
function wake() {
	for (const r of [...resting]) {
		const o = r.deref();
		let back = false;
		if (o)
			o._away = o._away.filter(([w, last]) => {
				const el = w.deref();
				if (!el)
					return false;
				if (!el.isConnected)
					return true;
				o._targets.set(el, last);
				back = true;
				return false;
			});
		if (!o || back || !o._away.length)
			resting.delete(r);
		if (o && back)
			o._active.add(o);
	}
}

/* An observer's targets: observe, unobserve, disconnect */
function watch(o, el) {
	check(el);
	o._away = o._away.filter(([w]) => w.deref() !== el);
	observedBy(el, o, true);
	if (o._targets.has(el))
		return;
	o._targets.set(el, undefined);
	o._active.add(o);
	observersNew = true;
}
function unwatch(o, el) {
	o._targets.delete(el);
	o._away = o._away.filter(([w]) => w.deref() !== el);
	observedBy(el, o, false);
	if (!o._targets.size)
		o._active.delete(o);
}
function unwatchAll(o) {
	for (const el of o._targets.keys())
		observedBy(el, o, false);
	for (const [w] of o._away) {
		const el = w.deref();
		if (el)
			observedBy(el, o, false);
	}
	o._targets.clear();
	o._away = [];
	o._active.delete(o);
}

/* Entries with their fields on the prototype, as browsers have them:
 * the intersection-observer polyfill (YouTube loads it) replaces an
 * IntersectionObserver whose entries lack intersectionRatio there */
const ENTRY = Symbol('entry');
class IntersectionObserverEntry {
	constructor(init) { this[ENTRY] = init; }
}
class ResizeObserverEntry {
	constructor(init) { this[ENTRY] = init; }
}
for (const [cls, names] of [[IntersectionObserverEntry, ['target', 'time',
    'isIntersecting', 'intersectionRatio', 'isVisible', 'boundingClientRect',
    'intersectionRect', 'rootBounds']], [ResizeObserverEntry, ['target',
    'contentRect', 'borderBoxSize', 'contentBoxSize',
    'devicePixelContentBoxSize']]])
	for (const n of names)
		Object.defineProperty(cls.prototype, n, { configurable: true,
			enumerable: true, get() { return this[ENTRY][n]; } });
let ioLast = '';

function margins(s, w, h) {
	const p = String(s || '0px').trim().split(/\s+/).map((v, i) => {
		const n = parseFloat(v) || 0;
		return /%$/.test(v) ? n / 100 * (i % 2 ? w : h) : n;
	});
	const [t = 0, r = t, b = t, l = r] = p;
	return [t, r, b, l];
}

/*
 * The rendering step of HTML's event loop, after the timers and their
 * microtasks, at most every 16 ms: animation frame callbacks, then
 * resize observers (again while sizes move, 4 rounds at most), then
 * intersection observers. Observers look only when the page scrolled
 * or changed, or a target is new. api.frame() returns ms until the
 * next step is due, -1 when nothing waits for one.
 */
const frameCallbacks = new Map();
let frameIds = 0, lastFrame = -1e9, observersNew = false;

function frameWanted() {
	if (frameCallbacks.size || observersNew)
		return true;
	if (!intersectionObservers.size && !resizeObservers.size &&
	    !resting.size)
		return false;
	return api.viewport().join() + ',' + api.gen() !== ioLast;
}

api.frame = function () {
	const now = api.now();
	if (!frameWanted())
		return frameCallbacks.size || intersectionObservers.size ||
			resizeObservers.size || resting.size ? 100 : -1;
	if (now - lastFrame < 16)
		return 16 - (now - lastFrame);
	lastFrame = now;
	const cbs = [...frameCallbacks.values()];
	frameCallbacks.clear();
	for (const fn of cbs) {
		try {
			fn(now);
		} catch (e) {
			report(e);
		}
		api.drain();	/* each callback, its microtasks */
	}
	observersNew = false;
	wake();
	for (let round = 0; round < 4; round++) {
		let moved = false;
		for (const ro of [...resizeObservers])
			moved = ro._check() || moved;
		if (!moved)
			break;
	}
	for (const ro of [...resizeObservers])
		rest(ro, resizeObservers);
	const v = api.viewport();
	for (const io of [...intersectionObservers]) {
		io._check(v);
		rest(io, intersectionObservers);
	}
	ioLast = v.join() + ',' + api.gen();
	return frameWanted() ? 16 : intersectionObservers.size ||
		resizeObservers.size || resting.size ? 100 : -1;
};

class IntersectionObserver {
	constructor(cb, opts = {}) {
		if (typeof cb !== 'function')
			throw new TypeError('IntersectionObserver: not a function');
		this._cb = cb;
		this.root = opts.root || null;
		this.rootMargin = opts.rootMargin || '0px';
		const t = [].concat(opts.threshold === undefined ? 0 :
			opts.threshold).map(Number).sort((a, b) => a - b);
		this.thresholds = t.length ? t : [0];
		this.delay = opts.delay || 0;
		this.trackVisibility = !!opts.trackVisibility;
		this._targets = new Map();	/* element: last threshold index */
		this._away = [];		/* [WeakRef, last] of those out */
		this._active = intersectionObservers;
	}
	observe(el) { watch(this, el); }
	unobserve(el) { unwatch(this, el); }
	disconnect() { unwatchAll(this); }
	_out(last) { return last === -1; }
	takeRecords() { return []; }
	_check(v) {
		const root = this.root && this.root !== document ?
			this.root.getBoundingClientRect() :
			new DOMRect(0, 0, v[0], v[1]);
		const [mt, mr, mb, ml] = margins(this.rootMargin, root.width,
			root.height);
		const rx0 = root.left - ml, ry0 = root.top - mt;
		const rx1 = root.right + mr, ry1 = root.bottom + mb;
		const entries = [];
		for (const [el, last] of this._targets) {
			const b = el.isConnected ? el.getBoundingClientRect() :
				new DOMRect();
			const x0 = Math.max(b.left, rx0), y0 = Math.max(b.top, ry0);
			const x1 = Math.min(b.right, rx1), y1 = Math.min(b.bottom, ry1);
			const meets = el.isConnected && x0 <= x1 && y0 <= y1 &&
				(b.width > 0 || b.height > 0 || x0 < x1 || y0 < y1);
			const area = b.width * b.height;
			const ratio = !meets ? 0 : area > 0 ?
				Math.max(0, x1 - x0) * Math.max(0, y1 - y0) / area : 1;
			let idx = meets ? 0 : -1;
			if (meets)
				for (const t of this.thresholds)
					if (ratio >= t)
						idx++;
			if (idx === last)
				continue;
			this._targets.set(el, idx);
			const inter = meets ? new DOMRect(x0, y0, x1 - x0, y1 - y0) :
				new DOMRect();
			entries.push(new IntersectionObserverEntry({ target: el,
				time: performance.now(), isIntersecting: meets,
				intersectionRatio: ratio, isVisible: meets,
				boundingClientRect: b, intersectionRect: inter,
				rootBounds: new DOMRect(rx0, ry0, rx1 - rx0, ry1 - ry0) }));
		}
		if (entries.length) {
			try {
				this._cb.call(this, entries, this);
			} catch (e) {
				report(e);
			}
			api.drain();
		}
	}
}
/* ResizeObserver: an entry at observe() and when a target's size
 * changes, looked at with the intersection observers */
const resizeObservers = new Set();

class ResizeObserver {
	constructor(cb) {
		if (typeof cb !== 'function')
			throw new TypeError('ResizeObserver: not a function');
		this._cb = cb;
		this._targets = new Map();	/* element: last "w h" */
		this._away = [];		/* [WeakRef, last] of those out */
		this._active = resizeObservers;
	}
	observe(el) { watch(this, el); }
	unobserve(el) { unwatch(this, el); }
	disconnect() { unwatchAll(this); }
	_out(last) { return last === '0 0'; }
	takeRecords() { return []; }
	_check() {
		const entries = [];
		for (const [el, last] of this._targets) {
			const r = el.isConnected ? el.getBoundingClientRect() :
				new DOMRect();
			const key = r.width + ' ' + r.height;
			if (key === last)
				continue;
			this._targets.set(el, key);
			const size = [{ inlineSize: r.width, blockSize: r.height }];
			entries.push(new ResizeObserverEntry({ target: el,
				contentRect: new DOMRect(0, 0, r.width, r.height),
				borderBoxSize: size, contentBoxSize: size,
				devicePixelContentBoxSize: size }));
		}
		if (entries.length) {
			try {
				this._cb.call(this, entries, this);
			} catch (e) {
				report(e);
			}
			api.drain();
		}
		return entries.length > 0;
	}
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
	mediaCapabilities: { value: {
		/* what MediaSource takes plays well: the media extension
		 * refuses what a small machine does not */
		decodingInfo(c = {}) {
			const ok = (m, v) => !m || api.mseSupported(String(m.contentType) +
				(v && m.width ? '; width=' + m.width + '; height=' +
				m.height : '') + (v && m.framerate ? '; framerate=' +
				m.framerate : ''));
			const yes = (!!c.video || !!c.audio) && ok(c.video, 1) &&
				ok(c.audio, 0);
			return Promise.resolve({ supported: yes, smooth: yes,
				powerEfficient: yes, configuration: c });
		},
		encodingInfo: (c) => Promise.resolve({ supported: false,
			smooth: false, powerEfficient: false, configuration: c }),
	}, enumerable: true },
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

/*
 * crypto.subtle: digests (SHA-1, SHA-256, SHA-384, SHA-512), HMAC
 * (sign, verify) and PBKDF2 (deriveBits, deriveKey: ALTCHA's proof of
 * work), raw keys. No cipher: an AES key can be derived, imported and
 * exported, not used.
 */
const keys = new WeakMap();
class CryptoKey {
	constructor() {
		throw new TypeError('Illegal constructor');
	}
	get type() { return 'secret'; }
	get extractable() { return keys.get(this).extractable; }
	get algorithm() { return keys.get(this).algorithm; }
	get usages() { return keys.get(this).usages; }
}

function algName(a) {
	return String(a && typeof a === 'object' ? a.name : a).toUpperCase();
}

function cryptoError(what, name) {
	return new DOMException(what, name);
}

function hashOf(a) {
	const h = algName(a);
	if (!/^SHA-(1|256|384|512)$/.test(h))
		throw cryptoError(h + ' is not a supported hash',
			'NotSupportedError');
	return h;
}

function bytesOf(b, what) {
	const a = asBytes(b);
	if (!a)
		throw new TypeError(what + ': not a BufferSource');
	return a;
}

/* A secret key of algorithm alg (name, its parameters) on bytes */
function newKey(alg, bytes, extractable, usages) {
	const name = algName(alg);
	let algorithm, can;
	usages = [...usages].map(String);
	if (name === 'PBKDF2') {
		if (extractable)
			throw cryptoError('PBKDF2 keys are not extractable',
				'SyntaxError');
		algorithm = { name };
		can = ['deriveKey', 'deriveBits'];
	} else if (name === 'HMAC') {
		if (!alg.hash)
			throw new TypeError('HMAC: a hash is required');
		if (!bytes.length)
			throw cryptoError('empty HMAC key', 'DataError');
		algorithm = { name, hash: { name: hashOf(alg.hash) },
			length: bytes.length * 8 };
		can = ['sign', 'verify'];
	} else if (/^AES-(GCM|CBC|CTR|KW)$/.test(name)) {
		if (![16, 24, 32].includes(bytes.length))
			throw cryptoError('AES keys are 128, 192 or 256 bits',
				'DataError');
		algorithm = { name, length: bytes.length * 8 };
		can = name === 'AES-KW' ? ['wrapKey', 'unwrapKey'] :
			['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'];
	} else {
		throw cryptoError(name + ' is not supported',
			'NotSupportedError');
	}
	if (!usages.length || usages.some((u) => !can.includes(u)))
		throw cryptoError('bad key usages for ' + name, 'SyntaxError');
	const k = Object.create(CryptoKey.prototype);
	keys.set(k, { extractable: !!extractable, algorithm,
		usages: [...new Set(usages)], bytes: bytes.slice() });
	return k;
}

/* key's record, if it is of algorithm name and allowed usage */
function keyFor(key, name, usage) {
	const k = keys.get(key);
	if (!k)
		throw new TypeError('not a CryptoKey');
	if (k.algorithm.name !== name || !k.usages.includes(usage))
		throw cryptoError('key not for ' + name + ' ' + usage,
			'InvalidAccessError');
	return k;
}

function pbkdf2Bits(alg, key, length, usage) {
	if (algName(alg) !== 'PBKDF2')
		throw cryptoError(algName(alg) + ' is not supported',
			'NotSupportedError');
	const k = keyFor(key, 'PBKDF2', usage);
	const iter = +alg.iterations;
	if (!length || length % 8 || !(iter >= 1))
		throw cryptoError('PBKDF2: bad length or iterations',
			'OperationError');
	const r = api.pbkdf2(hashOf(alg.hash), k.bytes,
		bytesOf(alg.salt, 'PBKDF2 salt'), iter, length / 8);
	if (!r)
		throw cryptoError('PBKDF2: cannot derive that', 'OperationError');
	return r;
}

/* each method answers a promise, its errors rejections */
const promised = (f) => function (...a) {
	try {
		return Promise.resolve(f(...a));
	} catch (e) {
		return Promise.reject(e);
	}
};

const subtle = {
	digest: promised((alg, data) => {
		const name = algName(alg);
		const r = api.digest(name, bytesOf(data, 'digest'));
		if (!r)
			throw cryptoError('digest: ' + name + ' is not supported',
				'NotSupportedError');
		return r;
	}),
	importKey: promised((format, data, alg, extractable, usages) => {
		if (format !== 'raw')
			throw cryptoError(format + ' keys are not supported',
				'NotSupportedError');
		return newKey(alg, bytesOf(data, 'importKey'), extractable,
			usages);
	}),
	exportKey: promised((format, key) => {
		const k = keys.get(key);
		if (!k)
			throw new TypeError('not a CryptoKey');
		if (format !== 'raw' || k.algorithm.name === 'PBKDF2')
			throw cryptoError(format + ' export is not supported',
				'NotSupportedError');
		if (!k.extractable)
			throw cryptoError('key not extractable',
				'InvalidAccessError');
		return bufferOf(k.bytes);
	}),
	sign: promised((alg, key, data) => {
		const k = keyFor(key, algName(alg), 'sign');
		if (k.algorithm.name !== 'HMAC')
			throw cryptoError('sign: not supported', 'NotSupportedError');
		return api.hmac(k.algorithm.hash.name, k.bytes,
			bytesOf(data, 'sign'));
	}),
	verify: promised((alg, key, sig, data) => {
		const k = keyFor(key, algName(alg), 'verify');
		if (k.algorithm.name !== 'HMAC')
			throw cryptoError('verify: not supported',
				'NotSupportedError');
		const a = new Uint8Array(api.hmac(k.algorithm.hash.name, k.bytes,
			bytesOf(data, 'verify')));
		const b = bytesOf(sig, 'verify');
		let d = a.length ^ b.length;
		for (let i = 0; i < a.length && i < b.length; i++)
			d |= a[i] ^ b[i];
		return d === 0;
	}),
	deriveBits: promised((alg, key, length) =>
		pbkdf2Bits(alg, key, length, 'deriveBits')),
	deriveKey: promised((alg, key, type, extractable, usages) => {
		const name = algName(type);
		let length = type && +type.length;
		if (name === 'HMAC' && !length)
			length = /^SHA-(384|512)$/.test(hashOf(type.hash)) ?
				1024 : 512;
		const r = pbkdf2Bits(alg, key, length, 'deriveKey');
		return newKey(type, new Uint8Array(r), extractable, usages);
	}),
};
for (const k of ['encrypt', 'decrypt', 'generateKey', 'wrapKey',
	'unwrapKey'])
	subtle[k] = () => Promise.reject(cryptoError(k + ' is not ' +
		'supported', 'NotSupportedError'));
crypto.subtle = subtle;

/*
 * Web Workers: each a thread of script.c, with dom.js of its own
 * without a document; messages copied (QuickJS's serialization). The
 * page fetches the worker's script, as browsers do.
 */
const workerObjs = new Map();
class Worker extends EventTarget {
	constructor(url, opts = {}) {
		super();
		const u = api.resolve(href, String(url)) || String(url);
		const id = api.workerNew(u, !!(opts && opts.type === 'module'));
		this._id = id;
		workerObjs.set(id, this);
		const fail = (e) => api.workerEvent(id, 'error',
			'cannot load the worker ' + u + (e ? ': ' + e : ''));
		const local = localURL(u);
		if (local) {
			addTimer(() => api.workerRun(id,
				new TextDecoder().decode(local.body)), 0, [], false);
			return;
		}
		fetch(u).then((r) => {
			if (!r.ok)
				throw new Error('HTTP ' + r.status);
			return r.text();
		}).then((t) => api.workerRun(id, t), fail);
	}
	postMessage(data) {
		api.workerPost(this._id, data);
	}
	terminate() {
		api.workerStop(this._id);
		workerObjs.delete(this._id);
	}
}
function handlerProps(proto, events) {
	for (const ev of events)
		Object.defineProperty(proto, 'on' + ev, {
			get() {
				return (this[HANDLERS] && this[HANDLERS][ev]) || null;
			},
			set(v) {
				(this[HANDLERS] || (this[HANDLERS] =
					Object.create(null)))[ev] =
					typeof v === 'function' ? v : null;
			},
			configurable: true,
		});
}
handlerProps(Worker.prototype, ['message', 'error', 'messageerror']);
/* script.c: worker id's message or error for the page; id 0, the
 * page's message in a worker */
api.workerEvent = function (id, type, data) {
	const t = id ? workerObjs.get(id) : g;
	if (!t)
		return;
	if (type === 'message')
		dispatch(t, new MessageEvent('message', { data }));
	else
		dispatch(t, new ErrorEvent('error', { message: String(data),
			cancelable: true }));
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
	crypto, CryptoKey,
	customElements, NodeFilter,
	localStorage: new Proxy(new Storage(), storageHandler),
	sessionStorage: new Proxy(new Storage(), storageHandler),
	EventTarget, Event, CustomEvent, UIEvent, MouseEvent, PointerEvent,
	WheelEvent, KeyboardEvent, FocusEvent, InputEvent, SubmitEvent,
	ProgressEvent, ErrorEvent, PopStateEvent, HashChangeEvent,
	MessageEvent, Node, CharacterData, Text, CDATASection, Comment,
	ProcessingInstruction,
	DocumentType, DocumentFragment, Document, HTMLDocument: Document,
	Element, SVGElement, SVGSVGElement, MathMLElement, Attr,
	DOMTokenList, CSSStyleDeclaration, DOMRect, DOMRectReadOnly,
	DOMException, MessageChannel, MessagePort, DOMParser, XMLSerializer,
	TreeWalker, NodeIterator: TreeWalker, Range,
	NodeList: Array, HTMLCollection: Array, URL, URLSearchParams,
	TextEncoder, TextDecoder, Blob, File, FormData, Headers, Request,
	ReadableStream, Worker,
	Response, fetch, XMLHttpRequest, AbortController, AbortSignal,
	Storage, MutationObserver, MutationRecord, IntersectionObserver,
	IntersectionObserverEntry, ResizeObserver, ResizeObserverEntry,
	PerformanceObserver, Window, Navigator, Screen, Location, History,
	btoa, atob, getComputedStyle, matchMedia,
	devicePixelRatio: 1,
	setTimeout: (fn, d, ...a) => addTimer(fn, d, a, false),
	setInterval: (fn, d, ...a) => addTimer(fn, d, a, true),
	clearTimeout: (id) => { timers.delete(id); },
	clearInterval: (id) => { timers.delete(id); },
	setImmediate: (fn, ...a) => addTimer(fn, 0, a, false),
	clearImmediate: (id) => { timers.delete(id); },
	requestAnimationFrame: (fn) => {
		frameCallbacks.set(++frameIds, fn);
		return frameIds;
	},
	cancelAnimationFrame: (id) => { frameCallbacks.delete(id); },
	requestIdleCallback: (fn) => addTimer(() => fn({ didTimeout: false,
		timeRemaining: () => 10 }), 1, [], false),
	cancelIdleCallback: (id) => { timers.delete(id); },
	queueMicrotask: (fn) => {
		THEN.call(RESOLVED, () => {
			try {
				fn();
			} catch (e) {
				report(e);
			}
		});
	},
	structuredClone: (v) => api.cloneValue(v),
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
	open: (url, target) => {
		if (!url)
			return null;
		if (target === undefined || newWindow(String(target)))
			api.navigate(api.resolve(href, String(url)) ||
				String(url), 2);
		else
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
/* MediaSource only with a media extension (FFmpeg's plays it), asked
 * without loading it: most pages never play anything */
if (api.mediaPresent())
	Object.assign(globals, { MediaSource, SourceBuffer, SourceBufferList });
globals.TimeRanges = TimeRanges;
globals.scroll = globals.scrollTo;
globals.origin = (() => {
	try {
		return new URL(href).origin;
	} catch (e) {
		return 'null';
	}
})();

/* addEventListener(...) called bare: this is undefined there, the
 * global object for browsers (Anubis's worker, pages' load listeners) */
for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent'])
	Object.defineProperty(g, k, { value: EventTarget.prototype[k].bind(g),
		writable: true, configurable: true });

/* A worker's global scope: what does not need a document */
if (api.worker) {
	class WorkerGlobalScope extends EventTarget {}
	class DedicatedWorkerGlobalScope extends WorkerGlobalScope {}
	class WorkerLocation {}
	const u = new URL(href);
	const wloc = Object.create(WorkerLocation.prototype);
	for (const k of ['href', 'origin', 'protocol', 'host', 'hostname',
		'port', 'pathname', 'search', 'hash'])
		Object.defineProperty(wloc, k, { value: u[k], enumerable: true });
	Object.defineProperty(wloc, 'toString', { value: () => u.href });
	const w = {
		self: g, location: wloc, name: '', WorkerGlobalScope,
		DedicatedWorkerGlobalScope, WorkerLocation,
		WorkerNavigator: Navigator,
		postMessage: (data) => api.workerPost(0, data),
		close: () => api.workerStop(0),
		importScripts: (...urls) => {
			for (const s of urls) {
				const a = api.resolve(href, String(s)) || String(s);
				const r = api.request('GET', a, null, null, 0);
				if (!r || r.status >= 400)
					throw new DOMException('importScripts: cannot ' +
						'load ' + a, 'NetworkError');
				(0, eval)(api.utf8Decode(r.body));
			}
		},
	};
	for (const k of ['navigator', 'performance', 'console', 'crypto',
		'CryptoKey',
		'isSecureContext', 'crossOriginIsolated', 'origin',
		'EventTarget', 'Event', 'CustomEvent', 'ErrorEvent',
		'MessageEvent', 'ProgressEvent', 'DOMException',
		'MessageChannel', 'MessagePort', 'URL', 'URLSearchParams',
		'TextEncoder', 'TextDecoder', 'Blob', 'File', 'FormData',
		'Headers', 'Request', 'Response', 'ReadableStream', 'fetch',
		'XMLHttpRequest', 'AbortController', 'AbortSignal', 'btoa',
		'atob', 'setTimeout', 'setInterval', 'clearTimeout',
		'clearInterval', 'queueMicrotask', 'structuredClone',
		'reportError'])
		w[k] = globals[k];
	for (const k of Object.keys(w))
		Object.defineProperty(g, k, { value: w[k], writable: true,
			configurable: true, enumerable: false });
	handlerProps(g, ['message', 'error', 'messageerror',
		'unhandledrejection', 'rejectionhandled']);
	Object.setPrototypeOf(g, DedicatedWorkerGlobalScope.prototype);
	return;
}
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
