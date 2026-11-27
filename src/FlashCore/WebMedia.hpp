#pragma once

#include <string_view>

namespace flash::web
{
inline constexpr std::string_view media_runtime = R"JS(const byteIndex = /^(0|[1-9]\d*)$/;

export class ByteArray {
    #bytes = new Uint8Array(32);
    #view = new DataView(this.#bytes.buffer);
    #length = 0;
    #position = 0;
    #shared = false;
    endian = "bigEndian";

    constructor(input, shared = false) {
        if (input != null) {
            const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
            if (shared) {
                if (bytes.byteLength > 0x10000000) throw new RangeError("Taille ByteArray invalide.");
                this.#bytes = bytes;
                this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
                this.#length = bytes.byteLength;
                this.#shared = true;
            } else this.append(bytes);
        }
        const methods = new Map();
        return new Proxy(this, {
            get: (target, key) => {
                if (typeof key === "string" && byteIndex.test(key))
                    return Number(key) < target.length ? target.#bytes[Number(key)] : undefined;
                const value = Reflect.get(target, key, target);
                if (typeof value !== "function") return value;
                let bound = methods.get(value);
                if (!bound) { bound = value.bind(target); methods.set(value, bound); }
                return bound;
            },
            set: (target, key, value) => {
                if (typeof key === "string" && byteIndex.test(key)) {
                    const index = Number(key);
                    target.#reserve(index + 1);
                    if (index >= target.#length) target.length = index + 1;
                    target.#bytes[index] = value;
                    return true;
                }
                return Reflect.set(target, key, value, target);
            },
        });
    }
    #reserve(size) {
        if (!Number.isSafeInteger(size) || size < 0 || size > 0x10000000) throw new RangeError("Taille ByteArray invalide.");
        if (size > this.#bytes.length || this.#shared) {
            const capacity = size > this.#bytes.length ? Math.max(size, Math.min(this.#bytes.length * 2, 0x10000000)) : this.#length;
            const bytes = new Uint8Array(Math.max(size, capacity));
            bytes.set(this.#bytes.subarray(0, this.#length));
            this.#bytes = bytes;
            this.#view = new DataView(bytes.buffer);
            this.#shared = false;
        }
    }
    get length() { return this.#length; }
    set length(size) {
        size = Number(size);
        this.#reserve(size);
        if (size > this.#length) this.#bytes.fill(0, this.#length, size);
        this.#length = size;
        this.#position = Math.min(this.#position, size);
    }
    get position() { return this.#position; }
    set position(value) { this.#position = Number(value) >>> 0; }
    get bytesAvailable() { return Math.max(0, this.#length - this.#position); }
    clear() { this.#length = this.#position = 0; }
    append(input) {
        this.#reserve(this.#length + input.length);
        this.#bytes.set(input, this.#length);
        this.#length += input.length;
    }
    bytes(offset = 0, end = this.#length) { return this.#bytes.slice(offset, end); }
    #out(data) {
        const end = this.#position + data.length;
        this.#reserve(end);
        if (this.#position > this.#length) this.#bytes.fill(0, this.#length, this.#position);
        this.#bytes.set(data, this.#position);
        this.#length = Math.max(this.#length, end);
        this.#position = end;
    }
    #in(size) {
        if (size > this.bytesAvailable) throw new RangeError("Fin du ByteArray.");
        const start = this.#position;
        this.#position += size;
        return this.#bytes.subarray(start, start + size);
    }
    #write(method, width, value) {
        const end = this.#position + width;
        this.#reserve(end);
        if (this.#position > this.#length) this.#bytes.fill(0, this.#length, this.#position);
        this.#view[method](this.#position, value, this.endian === "littleEndian");
        this.#length = Math.max(this.#length, end);
        this.#position = end;
    }
    #read(method, width) {
        if (width > this.bytesAvailable) throw new RangeError("Fin du ByteArray.");
        const value = this.#view[method](this.#position, this.endian === "littleEndian");
        this.#position += width;
        return value;
    }
    writeBoolean(value) { this.writeByte(value ? 1 : 0); }
    writeByte(value) { this.#write("setInt8", 1, value); }
    writeShort(value) { this.#write("setInt16", 2, value); }
    writeInt(value) { this.#write("setInt32", 4, value); }
    writeUnsignedInt(value) { this.#write("setUint32", 4, value); }
    writeFloat(value) { this.#write("setFloat32", 4, value); }
    writeDouble(value) { this.#write("setFloat64", 8, value); }
    writeUTF(value) {
        const data = new TextEncoder().encode(String(value));
        if (data.length > 65535) throw new RangeError("Chaîne UTF trop longue.");
        this.writeShort(data.length);
        this.#out(data);
    }
    writeUTFBytes(value) { this.#out(new TextEncoder().encode(String(value))); }
    writeBytes(value, offset = 0, length = 0) {
        if (typeof value?.bytes !== "function") throw new TypeError("ByteArray attendu.");
        offset = Number(offset) >>> 0;
        length = (Number(length) >>> 0) || value.length - offset;
        if (offset + length > value.length || length < 0) throw new RangeError("Plage ByteArray invalide.");
        this.#out(value.bytes(offset, offset + length));
    }
    readBoolean() { return this.readUnsignedByte() !== 0; }
    readByte() { return this.#read("getInt8", 1); }
    readUnsignedByte() { return this.#read("getUint8", 1); }
    readShort() { return this.#read("getInt16", 2); }
    readUnsignedShort() { return this.#read("getUint16", 2); }
    readInt() { return this.#read("getInt32", 4); }
    readUnsignedInt() { return this.#read("getUint32", 4); }
    readFloat() { return this.#read("getFloat32", 4); }
    readDouble() { return this.#read("getFloat64", 8); }
    readUTF() { return this.readUTFBytes(this.readUnsignedShort()); }
    readUTFBytes(length) { return new TextDecoder().decode(this.#in(Number(length) >>> 0)); }
    readMultiByte(length, encoding) { return new TextDecoder(encoding).decode(this.#in(Number(length) >>> 0)); }
    readBytes(destination, offset = 0, length = 0) {
        if (typeof destination?.writeBytes !== "function") throw new TypeError("ByteArray attendu.");
        const data = this.#in((Number(length) >>> 0) || this.bytesAvailable);
        const previous = destination.position;
        destination.position = offset;
        try { destination.writeBytes(new ByteArray(data)); }
        finally { destination.position = previous; }
    }
    toString() { return new TextDecoder().decode(this.bytes()); }
}

export class URLRequest {
    constructor(url = "") {
        this.url = url;
        this.method = "GET";
        this.requestHeaders = [];
        this.contentType = "application/x-www-form-urlencoded";
        this.data = null;
    }
}

export class URLRequestHeader {
    constructor(name = "", value = "") { Object.assign(this, { name, value }); }
}

export class URLVariables {
    constructor(source) { if (source != null) this.decode(source); }
    decode(source) {
        for (const [key, value] of new URLSearchParams(source)) {
            if (["__proto__", "constructor", "prototype", "decode", "toString"].includes(key)) continue;
            this[key] = Object.hasOwn(this, key) ? [].concat(this[key], value) : value;
        }
    }
    toString() {
        const entries = new URLSearchParams();
        for (const [key, value] of Object.entries(this))
            for (const item of [].concat(value)) entries.append(key, String(item));
        return entries.toString();
    }
}

export class SoundTransform {
    constructor(volume = 1, pan = 0) { Object.assign(this, { volume, pan }); }
}

export function mediaAdapters({ dispatcher, emit, resolve, socketEndpoint, problem }) {
    const openConnections = new Map(), channels = new Set();
    const soundChannels = new WeakSet();
    const messageId = () => Array.from(crypto.getRandomValues(new Uint32Array(4)), n => n.toString(16)).join("-");
    const requestOptions = (request, address = resolve) => {
        const url = address(request?.url ?? request);
        const method = String(request?.method ?? "GET").toUpperCase();
        const headers = new Headers();
        for (const header of request?.requestHeaders ?? []) headers.append(header.name, header.value);
        let body = request?.data;
        if (body != null && typeof body !== "string" && !(body instanceof ByteArray)) body = String(body);
        if (body != null && (method === "GET" || method === "HEAD")) {
            for (const [key, value] of new URLSearchParams(String(body))) url.searchParams.append(key, value);
            body = undefined;
        } else if (body != null) {
            if (!headers.has("content-type") && request.contentType) headers.set("content-type", request.contentType);
            if (body instanceof ByteArray) body = body.bytes();
        }
        return [url, { method, headers, body: body ?? undefined, credentials: "same-origin" }];
    };

    function urlLoader(request, address, movieForUrl, registerBytes) {
        const view = dispatcher();
        let pending;
        const loader = Object.assign(view, {
            data: null, dataFormat: "text", bytesLoaded: 0, bytesTotal: 0,
            close() { pending?.abort(); pending = null; },
            async load(request) {
                loader.close();
                loader.data = null;
                loader.bytesLoaded = loader.bytesTotal = 0;
                const operation = new AbortController();
                pending = operation;
                try {
                    const [url, options] = requestOptions(request, address);
                    const movie = loader.dataFormat === "binary" && options.method === "GET" ?
                        movieForUrl?.(request, url) : null;
                    const response = await fetch(movie ? new URL("source.bin", movie) : url, { ...options, signal: operation.signal });
                    if (pending !== operation) return;
                    emit(view, "open");
                    emit(view, "httpStatus", { status: response.status });
                    if (!response.ok) throw new Error("HTTP " + response.status + " · " + url);
                    loader.bytesTotal = Number(response.headers.get("content-length")) || 0;
                    const bytes = new Uint8Array(await response.arrayBuffer());
                    if (pending !== operation) return;
                    loader.bytesLoaded = bytes.length;
                    loader.bytesTotal ||= bytes.length;
                    emit(view, "progress", { bytesLoaded: loader.bytesLoaded, bytesTotal: loader.bytesTotal });
                    const text = loader.dataFormat === "binary" ? null : new TextDecoder().decode(bytes);
                    loader.data = loader.dataFormat === "variables" ? new URLVariables(text) : text ?? new ByteArray(bytes);
                    if (movie) registerBytes(loader.data, movie, url);
                    emit(view, "complete");
                } catch (error) {
                    if (pending === operation && error.name !== "AbortError") emit(view, "ioError", { text: error.message });
                } finally { if (pending === operation) pending = null; }
            },
        });
        if (request) queueMicrotask(() => loader.load(request));
        return loader;
    }

    function localConnection() {
        const view = dispatcher();
        let name, listener, leaseTimer;
        const token = messageId();
        const key = value => "flashstudio:local:" + value;
        const storage = (method, ...args) => {
            try { return localStorage[method](...args); }
            catch { return null; }
        };
        const lease = value => {
            try { return JSON.parse(storage("getItem", key(value))); } catch { return null; }
        };
        const claim = () => storage("setItem", key(name), JSON.stringify({ token, until: Date.now() + 180000 }));
        const notify = (success, code) => emit(view, "status", { level: success ? "status" : "error", code });
        const receive = (method, params) => {
            if (typeof method !== "string" || !/^[A-Za-z_$][\w$]*$/.test(method) ||
                ["constructor", "prototype", "__proto__", "connect", "close", "send", "bindView", "dispatchEvent",
                    "addEventListener", "removeEventListener", "hasEventListener"].includes(method) || !Array.isArray(params)) return false;
            const handler = view.client?.[method];
            if (typeof handler !== "function") return false;
            try { handler.apply(view.client, params); return true; }
            catch (error) { emit(view, "asyncError", { text: error.message }); return false; }
        };
        Object.assign(view, {
            client: view, domain: location.hostname, isPerUser: true,
            connect(value) {
                value = String(value);
                if (!value || value.includes(":") || name || openConnections.has(value) || lease(value)?.until > Date.now())
                    throw new TypeError("Port LocalConnection déjà utilisé ou invalide : " + value);
                name = value;
                openConnections.set(name, receive);
                claim();
                leaseTimer = setInterval(claim, 30000);
                listener = new BroadcastChannel(key(name));
                listener.onmessage = ({ data }) => {
                    if (data?.id && Array.isArray(data.params))
                        listener.postMessage({ reply: data.id, ok: receive(data.method, data.params) });
                };
                window.addEventListener("pagehide", view.close);
            },
            close() {
                if (!name) return;
                listener?.close();
                clearInterval(leaseTimer);
                if (lease(name)?.token === token) storage("removeItem", key(name));
                openConnections.delete(name);
                name = null;
                window.removeEventListener("pagehide", view.close);
            },
            send(recipient, method, ...params) {
                recipient = String(recipient);
                if (recipient.includes(":")) {
                    emit(view, "securityError", { text: "LocalConnection est limité à la même origine web." });
                    return;
                }
                const target = openConnections.get(recipient);
                if (target) {
                    queueMicrotask(() => { const ok = target(method, params); notify(ok, "LocalConnection.Send." + (ok ? "Success" : "Failed")); });
                    return;
                }
                const channel = new BroadcastChannel(key(recipient)), id = messageId();
                const done = success => { clearTimeout(timeout); channel.close(); notify(success, "LocalConnection.Send." + (success ? "Success" : "Failed")); };
                const timeout = setTimeout(() => done(false), 1200);
                channel.onmessage = ({ data }) => { if (data?.reply === id) done(data.ok); };
                try { channel.postMessage({ id, method, params }); }
                catch (error) { done(false); emit(view, "asyncError", { text: error.message }); }
            },
        });
        return view;
    }
)JS" R"JS(
    function socket(xml = false) {
        const view = dispatcher();
        let ws, incoming = new ByteArray(), outgoing = new ByteArray(), xmlPending = "";
        const xmlDecoder = new TextDecoder();
        const failed = text => {
            emit(view, "ioError", { text });
            problem(new Error(text));
        };
        Object.assign(view, {
            timeout: 20000, endian: "bigEndian",
            connect(host, port = 0) {
                if (ws) view.close();
                incoming.clear(); outgoing.clear(); xmlPending = "";
                try {
                    const endpoint = socketEndpoint(host, port);
                    if (!endpoint) throw new Error("Route WebSocket manquante pour " + host + ":" + port + " dans network.json.");
                    const connection = new WebSocket(endpoint);
                    ws = connection;
                    connection.binaryType = "arraybuffer";
                    const expiry = setTimeout(() => {
                        if (ws === connection && connection.readyState === WebSocket.CONNECTING) {
                            connection.close();
                            failed("Délai de connexion WebSocket dépassé · " + endpoint);
                        }
                    }, view.timeout);
                    connection.onopen = () => { clearTimeout(expiry); if (ws === connection) emit(view, "connect"); };
                    connection.onclose = () => { clearTimeout(expiry); if (ws === connection) { ws = null; emit(view, "close"); } };
                    connection.onerror = () => { if (ws === connection) failed("Échec de connexion WebSocket · " + endpoint); };
                    connection.onmessage = ({ data }) => {
                        if (ws !== connection) return;
                        if (xml) {
                            xmlPending += typeof data === "string" ? data : xmlDecoder.decode(data, { stream: true });
                            if (xmlPending.length > 0x100000) { view.close(); emit(view, "ioError", { text: "Trame XMLSocket trop grande." }); return; }
                            const messages = xmlPending.split("\0");
                            xmlPending = messages.pop();
                            for (const text of messages) emit(view, "data", { text, data: text });
                        } else {
                            const bytes = typeof data === "string" ? new TextEncoder().encode(data) : new Uint8Array(data);
                            if (incoming.position) incoming = new ByteArray(incoming.bytes(incoming.position));
                            incoming.append(bytes);
                            emit(view, "socketData", { bytesLoaded: bytes.length, bytesTotal: incoming.bytesAvailable });
                        }
                    };
                } catch (error) { queueMicrotask(() => failed(error.message)); }
            },
            close() { ws?.close(); },
            flush() {
                if (!view.connected) throw new Error("Socket déconnecté.");
                if (outgoing.length) { ws.send(outgoing.bytes()); outgoing.clear(); }
            },
        });
        if (xml) view.send = value => {
            if (!view.connected) throw new Error("Socket déconnecté.");
            ws.send(String(value) + "\0");
        };
        Object.defineProperties(view, {
            connected: { get: () => ws?.readyState === WebSocket.OPEN },
            bytesAvailable: { get: () => incoming.bytesAvailable },
            bytesPending: { get: () => outgoing.length + (ws?.bufferedAmount ?? 0) },
        });
        for (const key of Object.getOwnPropertyNames(ByteArray.prototype)) {
            if (key.startsWith("read")) view[key] = (...args) => { incoming.endian = view.endian; return incoming[key](...args); };
            if (key.startsWith("write")) view[key] = (...args) => { outgoing.endian = view.endian; return outgoing[key](...args); };
        }
        return view;
    }

    const decodedSounds = new Map();
    const audioBudget = 24 * 1024 * 1024;
    const mediaVolumes = new Map();
    const volumeOf = value => Math.max(0, Math.min(1, Number(value?.volume) || 0));
    let context, masterGain, masterTransform = new SoundTransform(), decodedBytes = 0;
    function openAudio() {
        if (typeof AudioContext !== "function") return null;
        if (!context) {
            context = new AudioContext();
            masterGain = context.createGain();
            masterGain.gain.value = volumeOf(masterTransform);
            masterGain.connect(context.destination);
        }
        if (context.state === "suspended") context.resume().catch(() => {});
        return context;
    }
    function soundBuffer(url, ctx) {
        const cached = decodedSounds.get(url);
        if (cached) {
            decodedSounds.delete(url);
            decodedSounds.set(url, cached);
            return cached.result;
        }
        const entry = { bytes: 0 };
        entry.result = fetch(url).then(response => {
            if (!response.ok || Number(response.headers.get("Content-Length")) > 4 * 1024 * 1024)
                throw new Error("Le son est indisponible pour le décodage local.");
            return response.arrayBuffer();
        }).then(bytes => ctx.decodeAudioData(bytes)).then(buffer => {
            if (decodedSounds.get(url) !== entry) return buffer;
            entry.bytes = buffer.length * buffer.numberOfChannels * 4;
            decodedBytes += entry.bytes;
            while (decodedBytes > audioBudget || decodedSounds.size > 64) {
                const first = decodedSounds.keys().next().value;
                decodedBytes -= decodedSounds.get(first).bytes;
                decodedSounds.delete(first);
            }
            return buffer;
        }, error => {
            if (decodedSounds.get(url) === entry) decodedSounds.delete(url);
            throw error;
        });
        decodedSounds.set(url, entry);
        return entry.result;
    }

    function sound(request, address, embedded) {
        const view = dispatcher();
        let source = embedded?.url, probe, decodedLength = 0;
        const timing = embedded?.timing;
        const sound = Object.assign(view, {
            isBuffering: false,
            load(request) {
                sound.close();
                embedded = null;
                decodedLength = 0;
                const [url] = requestOptions(request, address);
                source = url.href;
                probe = new Audio();
                const current = probe;
                sound.isBuffering = true;
                current.preload = "auto";
                current.addEventListener("canplay", () => {
                    if (current !== probe) return;
                    sound.isBuffering = false;
                    emit(view, "complete");
                }, { once: true });
                current.addEventListener("error", () => {
                    if (current === probe) { sound.isBuffering = false; emit(view, "ioError", { text: "Son introuvable : " + source }); }
                }, { once: true });
                current.src = source;
                current.load();
            },
            close() {
                if (probe) { probe.pause(); probe.removeAttribute("src"); probe.load(); probe = null; }
                sound.isBuffering = false;
            },
            play(start = 0, loops = 0, transform = new SoundTransform(), end = 0) {
                if (!source) return null;
                const channel = dispatcher(), url = new URL(source);
                soundChannels.add(channel);
                let remaining = Math.max(0, Number(loops) | 0);
                const offset = Math.max(0, (Number(start) || 0) / 1000);
                let settings = transform, stopped = false, audio, loop, gain, playedAt, duration;
                const volume = () => volumeOf(settings);
                const syncMediaVolume = () => { audio.volume = volume() * volumeOf(masterTransform); };
                channel.stop = () => {
                    if (stopped) return;
                    stopped = true;
                    if (audio) { audio.pause(); audio.removeAttribute("src"); audio.load(); }
                    if (loop) { loop.disconnect(); gain.disconnect(); loop.stop(); }
                    channels.delete(channel);
                    mediaVolumes.delete(channel);
                };
                const finish = () => { channel.stop(); emit(channel, "soundComplete"); };
                Object.defineProperties(channel, {
                    position: { get: () => loop ? (offset + (loop.loop ? (context.currentTime - playedAt) % duration :
                        Math.min(duration, context.currentTime - playedAt))) * 1000 :
                        (audio?.currentTime ?? offset) * 1000 },
                    soundTransform: { get: () => settings, set: value => {
                        settings = value;
                        if (audio) syncMediaVolume();
                        if (gain) gain.gain.value = volume();
                    } },
                });
                const playMedia = () => {
                    if (stopped) return;
                    audio = new Audio(url.href);
                    mediaVolumes.set(channel, syncMediaVolume);
                    syncMediaVolume();
                    const seek = () => { audio.currentTime = offset; };
                    const play = () => audio.play().catch(error => {
                        if (stopped) return;
                        channel.stop();
                        emit(view, "ioError", { text: error.message });
                    });
                    if (offset) audio.addEventListener("loadedmetadata", seek, { once: true });
                    const next = () => {
                        if (stopped) return;
                        if (remaining-- > 0) { seek(); play(); }
                        else finish();
                    };
                    audio.addEventListener("ended", next);
                    if (end > 0) audio.addEventListener("timeupdate", () => { if (audio.currentTime * 1000 >= end) next(); });
                    play();
                };
                channels.add(channel);
                if (url.origin === location.origin && /\/assets\/[^/]+\.(mp3|wav|ogg)$/i.test(url.pathname) && openAudio()) {
                    soundBuffer(url.href, context).then(buffer => {
                        if (stopped) return;
                        // Flash embeds PCM sample counts and the decoder's MP3 priming offset separately.
                        const [rate, samples, priming] = embedded && timing || [];
                        const skip = rate ? Math.max(0, priming / rate) : 0;
                        const bound = samples && rate ? Math.min(buffer.duration, skip + samples / rate) : buffer.duration;
                        const first = Math.round((skip + offset) * buffer.sampleRate) / buffer.sampleRate;
                        const last = Math.round(Math.min(bound, end > 0 ? skip + end / 1000 : bound) * buffer.sampleRate) / buffer.sampleRate;
                        decodedLength = Math.max(0, bound - skip) * 1000;
                        duration = last - first;
                        if (!(duration > 0)) { playMedia(); return; }
                        const source = context.createBufferSource();
                        source.buffer = buffer;
                        source.loop = remaining > 0;
                        source.loopStart = first;
                        source.loopEnd = last;
                        gain = context.createGain();
                        gain.gain.value = volume();
                        source.connect(gain).connect(masterGain);
                        source.onended = () => { if (!stopped) finish(); };
                        playedAt = context.currentTime;
                        source.start(playedAt, first, duration * (remaining + 1));
                        loop = source;
                    }, playMedia).catch(playMedia);
                } else playMedia();
                return channel;
            },
        });
        Object.defineProperties(sound, {
            length: { get: () => decodedLength || (embedded && timing?.[0] ? timing[1] / timing[0] * 1000 :
                Number.isFinite(probe?.duration) ? probe.duration * 1000 : 0) },
            url: { get: () => source ?? null },
        });
        if (request) sound.load(request);
        return sound;
    }

    return {
        urlLoader, localConnection, socket, sound, requestOptions,
        get masterTransform() { return new SoundTransform(masterTransform.volume, masterTransform.pan); },
        set masterTransform(value) {
            masterTransform = new SoundTransform(value?.volume, value?.pan);
            if (masterGain) masterGain.gain.value = volumeOf(masterTransform);
            for (const sync of mediaVolumes.values()) sync();
        },
        get masterVolume() { return volumeOf(masterTransform); },
        embeddedSound(url, timing) { return sound(null, undefined, { url, timing }); },
        resumeAudio: openAudio,
        isSoundChannel(value) { return soundChannels.has(value); },
        stopAll() { for (const channel of channels) channel.stop(); },
        navigate(request, target = "_self", address) {
            try {
                const [url] = requestOptions(request, address);
                if (target === "_self") location.assign(url);
                else window.open(url.href, target, "noopener");
            } catch (error) { problem(error); }
        },
    };
}
)JS";
}
