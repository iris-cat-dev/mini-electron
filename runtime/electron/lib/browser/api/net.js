const fs = require('node:fs');
const path = require('node:path');
const { fileURLToPath } = require('node:url');
const { Readable } = require('node:stream');

const {app} = require('electron');

const hasCrypto = (process.versions.openssl != '0');

const http = require('http');
const https = hasCrypto ? require('https') : null;

const URL = require('url').URL;

function ClientRequest(options) {
    this.req = null;
    if ("string" == typeof options) {
        const urlObj = new URL('https://example.org');
        if (hasCrypto && "https:" == urlObj.protocol)
            this.req = https.request(options);
    } else if (hasCrypto && "https:" == options.protocol) {
        this.req = https.request(options);
    }
    if (!this.req)
        this.req = http.request(options);
}
//Object.setPrototypeOf(ClientRequest.prototype, EventEmitter.prototype);

ClientRequest.prototype.on = function(evt, callback) {
    this.req.on(evt, callback);
    return this;
}

ClientRequest.prototype.once = function(evt, callback) {
    this.req.once(evt, callback);
    return this;
}

ClientRequest.prototype.chunkedEncoding = false;

ClientRequest.prototype.setHeader = function(name, value) {
    this.req.setHeader(name, value);
}

ClientRequest.prototype.getHeader = function(name) {
    return this.req.getHeader(name);
}

ClientRequest.prototype.removeHeader = function(name) {
    this.req.removeHeader(name);
}

ClientRequest.prototype.write = function(chunk, encoding, callback) {
    this.req.write(chunk, encoding, callback);
}

ClientRequest.prototype.end = function(chunk, encoding, callback) {
    this.req.end(chunk, encoding, callback);
}

ClientRequest.prototype.abort = function() {
    this.req.abort();
}

ClientRequest.prototype.followRedirect = function() {
    ;
}

function Net() {
}

Net.request = function(options) {
    if (options.session)
        options.session = null; // electron的session和nodejs的，意义不一样。暂时不支持electron net的session
    return new ClientRequest(options);
}

Net.fetch = async function(input, options) {
    const request = new Request(input, options);
    if (new URL(request.url).protocol !== 'file:')
        return globalThis.fetch(request);
    if (request.method !== 'GET' && request.method !== 'HEAD')
        return new Response(null, { status: 405 });
    const file = fileURLToPath(request.url);
    let stat;
    try {
        stat = await fs.promises.stat(file);
    } catch (error) {
        if (error.code === 'ENOENT' || error.code === 'ENOTDIR')
            return new Response('Not found', { status: 404 });
        throw error;
    }
    if (!stat.isFile())
        return new Response('Not found', { status: 404 });
    const types = {
        '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
        '.mjs': 'text/javascript', '.json': 'application/json',
        '.wasm': 'application/wasm', '.svg': 'image/svg+xml',
        '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
        '.ico': 'image/x-icon', '.woff': 'font/woff', '.woff2': 'font/woff2',
        '.ttf': 'font/ttf', '.txt': 'text/plain',
    };
    const headers = {
        'content-type': types[path.extname(file).toLowerCase()] || 'application/octet-stream',
        'content-length': String(stat.size),
    };
    return new Response(request.method === 'HEAD' ? null :
        Readable.toWeb(fs.createReadStream(file)), { headers });
};

Net.isOnline = function() {
    return app.isOnline();
}

Object.defineProperty(Net, 'online', {
    get:function() {
        return app.isOnline();
    }
})

exports.net = Net;

