
const binding = process._linkedBinding('electron_browser_protocol');
const Protocol = binding.Protocol;
const protocol = new Protocol(onLoadUrlBegin);

var handlerToIdMap = {};
var idGen = 0;
var schemeToIdMap = {};

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
    var id = ++idGen;
    handlerToIdMap[id] = handler;
    this._registerProtocol(scheme, id, type);
    schemeToIdMap[scheme] = id;
    if (completion)
        completion(null);
}

Protocol.prototype.handle = function(scheme, handler) {
    if (typeof handler !== 'function')
        throw new TypeError('Protocol handler must be a function');
    if (this._isProtocolHandled(scheme))
        throw new Error('Protocol is already handled: ' + scheme);
    this.registerProtocol(scheme, (request, finish) => {
        Promise.resolve().then(() => handler(new Request(request.url, {
            method: request.method, headers: request.headers
        }))).then(async response => {
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
    delete handlerToIdMap[schemeToIdMap[scheme]];
    delete schemeToIdMap[scheme];

    this._unregisterProtocol(scheme);
    if (completion)
        completion(null);
}

Protocol.prototype.isProtocolHandled = function(scheme, callback) {
    var b = this._isProtocolHandled(scheme);
    callback(b);
}

Protocol.prototype.interceptFileProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion);
}

Protocol.prototype.interceptStringProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion);
}

Protocol.prototype.interceptBufferProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion);
}

Protocol.prototype.interceptHttpProtocol = function(scheme, handler, completion) {
    this.registerProtocol(scheme, handler, completion);
}

Protocol.prototype.uninterceptProtocol = function(scheme, completion) {
    this.unregisterProtocol(scheme, completion);
}

exports.protocol = protocol;