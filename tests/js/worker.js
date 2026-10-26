importScripts('worker-lib.js');
onmessage = async (e) => {
	const d = e.data;
	postMessage({ got: d.n, sum: sum(d.bytes), self: self === globalThis,
		scope: self instanceof WorkerGlobalScope, file: location.pathname.replace(/.*\//, '') });
	const h = await crypto.subtle.digest('SHA-1', new TextEncoder().encode('abc'));
	console.log('in the worker: sha1 ' + new Uint8Array(h)[0].toString(16));
	setTimeout(() => postMessage({ sha1: [...new Uint8Array(h).slice(0, 4)], done: true }), 10);
};
