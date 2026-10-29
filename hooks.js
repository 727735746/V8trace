"use strict";

function installDiagnosticHooks(bindingName, maxEvents, recordingGate) {
    "use strict";
    const global = globalThis;
    const emitBinding = global[bindingName];
    if (typeof emitBinding !== "function") return;
    delete global[bindingName];
    const apply = Reflect.apply;
    const stringify = JSON.stringify;
    const keys = Object.keys;
    const descriptorOf = Object.getOwnPropertyDescriptor;
    const define = Object.defineProperty;
    const isArray = Array.isArray;
    const finite = Number.isFinite;
    const is = Object.is;
    const bigintToString = BigInt.prototype.toString;
    const weakGet = WeakMap.prototype.get;
    const weakSet = WeakMap.prototype.set;
    const references = new WeakMap();
    let referenceId = 0;
    let sequence = 0;
    let spanSequence = 0;
    let currentSpan = null;
    let recording = false;
    let exhausted = false;
    let readySent = false;

    function enabled() {
        if (recordingGate === undefined) return true;
        try { return recordingGate === true ? apply(emitBinding, global, []) === true : apply(recordingGate, global, []) === true; }
        catch (_) { return false; }
    }

    function announceReady() {
        if (readySent || !enabled()) return;
        readySent = emit({ kind: "ready", observation: "diagnostic-js-hooks", identityChanged: true, stackCaptured: false });
    }

    function encodeKnown(value) {
        if (value === null) return "null";
        if (typeof value === "string") return stringify(value);
        if (typeof value === "boolean") return value ? "true" : "false";
        if (typeof value === "number") return finite(value) ? `${value}` : "null";
        if (isArray(value)) {
            let text = "[";
            for (let index = 0; index < value.length; index += 1) text += `${index ? "," : ""}${encodeKnown(value[index])}`;
            return `${text}]`;
        }
        const names = keys(value);
        let text = "{";
        for (let index = 0; index < names.length; index += 1) {
            const key = names[index];
            text += `${index ? "," : ""}${stringify(key)}:${encodeKnown(value[key])}`;
        }
        return `${text}}`;
    }

    function valueRecord(value) {
        const type = typeof value;
        if (value === null) return { type: "null" };
        if (type === "undefined") return { type };
        if (type === "string") return { type, value, length: value.length, truncated: false };
        if (type === "boolean") return { type, value };
        if (type === "number") return { type, value: is(value, -0) ? "-0" : finite(value) ? value : value !== value ? "NaN" : value > 0 ? "Infinity" : "-Infinity" };
        if (type === "bigint") return { type, value: apply(bigintToString, value, []) };
        if (type === "symbol") return { type, opaque: true };
        let id = apply(weakGet, references, [value]);
        if (!id) { id = `object:${++referenceId}`; apply(weakSet, references, [value, id]); }
        return { type: "ref", id, callable: type === "function" };
    }

    function emit(record) {
        if (recording || exhausted || !enabled()) return false;
        recording = true;
        try {
            if (sequence >= maxEvents) {
                const accepted = apply(emitBinding, global, [encodeKnown({ kind: "gap", reason: "hook-event-budget", producerSeq: sequence + 1 })]);
                if (recordingGate === true && accepted !== true) return false;
                sequence += 1;
                exhausted = true;
            } else {
                define(record, "producerSeq", { value: sequence + 1, enumerable: true });
                const accepted = apply(emitBinding, global, [encodeKnown(record)]);
                if (recordingGate === true && accepted !== true) return false;
                sequence += 1;
            }
            return true;
        } catch (_) {
            exhausted = true;
            return false;
        } finally { recording = false; }
    }

    function invoke(original, receiver, args, binding, operation) {
        if (recording || exhausted || !enabled()) return apply(original, receiver, args);
        announceReady();
        if (!readySent) return apply(original, receiver, args);
        const spanId = `hook:${++spanSequence}`;
        const parentSpanId = currentSpan;
        const values = [];
        for (let index = 0; index < args.length; index += 1) define(values, index, { value: valueRecord(args[index]), enumerable: true, configurable: true, writable: true });
        if (!emit({ kind: "enter", binding, operation, spanId, parentSpanId, receiver: valueRecord(receiver), arguments: values })) return apply(original, receiver, args);
        currentSpan = spanId;
        try {
            const result = apply(original, receiver, args);
            emit({ kind: "return", binding, operation, spanId, parentSpanId, result: valueRecord(result) });
            return result;
        } catch (error) {
            emit({ kind: "throw", binding, operation, spanId, parentSpanId, exception: valueRecord(error) });
            throw error;
        } finally { currentSpan = parentSpanId; }
    }

    function wrap(original, binding, operation) {
        const wrapped = function () { return invoke(original, this, arguments, binding, operation); };
        for (const key of ["name", "length"]) {
            const descriptor = descriptorOf(original, key);
            if (descriptor && "value" in descriptor) define(wrapped, key, descriptor);
        }
        return wrapped;
    }

    function install(owner, interfaceName, members) {
        if (!owner) return;
        for (let index = 0; index < members.length; index += 1) {
            const member = members[index];
            const descriptor = descriptorOf(owner, member);
            if (!descriptor || !descriptor.configurable) continue;
            const binding = `${interfaceName}.${member}`;
            if (typeof descriptor.get === "function") descriptor.get = wrap(descriptor.get, binding, "get");
            if (typeof descriptor.set === "function") descriptor.set = wrap(descriptor.set, binding, "set");
            if (typeof descriptor.value === "function") descriptor.value = wrap(descriptor.value, binding, "call");
            try { define(owner, member, descriptor); emit({ kind: "installed", binding }); }
            catch (_) { emit({ kind: "install-failed", binding }); }
        }
    }

    const specifications = [
        ["Navigator", ["userAgent", "platform", "language", "languages", "hardwareConcurrency", "deviceMemory", "webdriver", "cookieEnabled", "maxTouchPoints", "userAgentData"]],
        ["WorkerNavigator", ["userAgent", "platform", "language", "languages", "hardwareConcurrency", "deviceMemory", "userAgentData"]],
        ["Screen", ["width", "height", "availWidth", "availHeight", "colorDepth", "pixelDepth"]],
        ["Document", ["createElement", "createElementNS", "querySelector", "querySelectorAll", "getElementById", "URL", "referrer", "visibilityState", "readyState"]],
        ["Element", ["getAttribute", "setAttribute", "querySelector", "querySelectorAll", "getBoundingClientRect"]],
        ["HTMLCanvasElement", ["getContext", "toDataURL", "toBlob"]],
        ["CanvasRenderingContext2D", ["fillText", "measureText", "getImageData", "putImageData", "fillRect", "drawImage"]],
        ["Storage", ["getItem", "setItem", "removeItem", "clear", "key"]],
        ["XMLHttpRequest", ["open", "send", "setRequestHeader", "getResponseHeader", "getAllResponseHeaders"]],
        ["Crypto", ["getRandomValues", "randomUUID"]],
        ["SubtleCrypto", ["digest", "encrypt", "decrypt", "sign", "verify", "importKey", "deriveBits"]]
    ];
    for (let index = 0; index < specifications.length; index += 1) {
        const [name, members] = specifications[index];
        const descriptor = descriptorOf(global, name);
        const constructor = descriptor && descriptor.value;
        if (typeof constructor === "function") install(constructor.prototype, name, members);
    }
    if (typeof global.Document === "function") install(global.Document.prototype, "Document", ["cookie"]);
    install(global, "WindowOrWorkerGlobalScope", ["fetch", "setTimeout", "setInterval", "queueMicrotask", "atob", "btoa"]);
    install(Math, "Math", ["random"]);
    announceReady();
}

function createHookSource(bindingName, options = {}) {
    return `(${installDiagnosticHooks.toString()})(${JSON.stringify(bindingName)},${options.maxEvents || 20000});\n//# sourceURL=v8trace://diagnostic-hooks`;
}

module.exports = { createHookSource };
