
const binding = process._linkedBinding('electron_browser_protocol');
const Protocol = binding.Protocol;
const protocol = new Protocol(onLoadUrlBegin);

var handlerToIdMap = {};
var idGen = 0;
var schemeToIdMap = {};

function normalizeScheme(scheme) {
    if (typeof scheme !== 'string')
        throw new TypeError('Protocol scheme must be a string');
    const normalized = scheme.replace(/:$/, '').toLowerCase();
    if (!/^[a-z][a-z0-9+.-]*$/.test(normalized))
        throw new TypeError(`Invalid protocol scheme: ${scheme}`);
    return normalized;
}

function onLoadUrlBegin(id, request, nativeCallbackInfo) {
    var handler = handlerToIdMap[id];
    if (!handler) {
        protocol.onHandlerFinish(request, nativeCallbackInfo);
        return;
    }

    let finished = false;
    const finish = (response) => {
        if (finished) return;
        finished = true;
        protocol.onHandlerFinish(response, nativeCallbackInfo);
    };
    try {
        handler(request, finish);
    } catch (error) {
        finish({ error: -2 });
        console.error(error);
    }
}

Protocol.prototype.registerProtocol = function(scheme, handler, completion, type) {
    scheme = normalizeScheme(scheme);
    if (typeof handler !== 'function')
        throw new TypeError('Protocol handler must be a function');
    if (this._isProtocolHandled(scheme)) {
        const error = new Error(`Protocol is already handled: ${scheme}`);
        if (completion) {
            completion(error);
            return;
        }
        throw error;
    }
    const id = ++idGen;
    if (!this._registerProtocol(scheme, id, type || 'buffer')) {
        const error = new Error(`Unable to register protocol: ${scheme}`);
        if (completion) {
            completion(error);
            return;
        }
        throw error;
    }
    handlerToIdMap[id] = handler;
    schemeToIdMap[scheme] = id;
    if (completion)
        completion(null);
}

Protocol.prototype.handle = function(scheme, handler) {
    scheme = normalizeScheme(scheme);
    if (typeof handler !== 'function')
        throw new TypeError('Protocol handler must be a function');
    if (this._isProtocolHandled(scheme))
        throw new Error('Protocol is already handled: ' + scheme);
    this.registerProtocol(scheme, (request, finish) => {
        Promise.resolve().then(() => {
            const init = { method: request.method, headers: request.headers };
            if (request.body != null &&
                request.method !== 'GET' && request.method !== 'HEAD') {
                init.body = request.body;
                init.duplex = 'half';
            }
            return handler(new Request(request.url, init));
        }).then(async response => {
            if (!(response instanceof Response))
                throw new TypeError('Protocol handler must return a Response');
            finish({
                data: Buffer.from(await response.arrayBuffer()),
                mimeType: response.headers.get('content-type') || '',
                statusCode: response.status,
                statusText: response.statusText,
                headers: Object.fromEntries(response.headers),
            });
        }).catch(error => {
            finish({ error: -2 });
            console.error(error);
        });
    }, undefined, 'response');
};

Protocol.prototype.unhandle = function(scheme) {
    this.unregisterProtocol(scheme);
};

Protocol.prototype.registerFileProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, "file");
}

Protocol.prototype.registerBufferProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, "buffer");
}

Protocol.prototype.registerStringProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, "string");
}

Protocol.prototype.registerHttpProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, "http");
}

Protocol.prototype.unregisterProtocol = function(scheme, completion) {
    scheme = normalizeScheme(scheme);
    const id = schemeToIdMap[scheme];
    if (id !== undefined) {
        delete handlerToIdMap[id];
        delete schemeToIdMap[scheme];
    }
    this._unregisterProtocol(scheme);
    if (completion)
        completion(null);
}

Protocol.prototype.isProtocolHandled = function(scheme, callback) {
    const handled = this._isProtocolHandled(normalizeScheme(scheme));
    if (callback)
        callback(handled);
    return handled;
}

Protocol.prototype.interceptFileProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, 'file');
}

Protocol.prototype.interceptStringProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, 'string');
}

Protocol.prototype.interceptBufferProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, 'buffer');
}

Protocol.prototype.interceptHttpProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion, 'http');
}

Protocol.prototype.uninterceptProtocol = function(scheme, completion) {
    this.unregisterProtocol(scheme, completion);
}

exports.protocol = protocol;