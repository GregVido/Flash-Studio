#pragma once
#include <string_view>

namespace flash::web
{
inline constexpr std::string_view avm1_runtime = R"JS(
const properties = ["_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha", "_visible",
    "_width", "_height", "_rotation", "_target", "_framesloaded", "_name", "_droptarget", "_url", "_highquality",
    "_focusrect", "_soundbuftime", "_quality", "_xmouse", "_ymouse"];
const aliases = new Map(Object.entries({
    _x: "x", _y: "y", _rotation: "rotation", _width: "width", _height: "height", _visible: "visible",
    _name: "name", _xmouse: "mouseX", _ymouse: "mouseY",
    _xscale: "scaleX", _yscale: "scaleY", _alpha: "alpha"
}));
const percentages = new Set(["_xscale", "_yscale", "_alpha"]);
const protectedKeys = new Set(["__proto__", "constructor"]);
const has = (value, name) => value != null && Object.hasOwn(value, name);

export function createLegacyRuntime(hooks) {
    const wrappers = new WeakMap(), objects = new WeakMap(), globals = new WeakMap();
    const keys = new Set();
    let lastKey = 0, callDepth = 0;
    const Key = { isDown: value => keys.has(Number(value)), getCode: () => lastKey,
        LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, SPACE: 32, ENTER: 13,
        SHIFT: 16, CONTROL: 17, ESCAPE: 27, TAB: 9, BACKSPACE: 8, DELETEKEY: 46 };
    const getRoot = clip => {
        while (clip?.parent && clip.parent.bundle === clip.bundle) clip = clip.parent;
        return clip;
    };
    function globalScope(clip) {
        let scope = globals.get(clip.bundle);
        if (scope) return scope;
        scope = Object.assign(Object.create(null), {
            Math, Key, Object, Array, Number, String, Boolean, Date, parseInt, parseFloat, isNaN, isFinite,
            NaN, Infinity,
            Sound: function() { return hooks.sound(clip); },
            Color: function(target) { return hooks.color(objects.get(target)?.view); }
        });
        globals.set(clip.bundle, scope);
        return scope;
    }
    function wrap(clip, view = clip?.view) {
        if (!view) return undefined;
        if (wrappers.has(view)) return wrappers.get(view);
        const values = clip?.variables ?? Object.create(null);
        const methods = new Map();
        const result = new Proxy(values, {
            get(_, key) {
                if (key === Symbol.toPrimitive) return () => targetPath(result);
                if (typeof key !== "string" || protectedKeys.has(key)) return undefined;
                if (key === "_root" || key === "_level0") return wrap(getRoot(clip ?? hooks.owner(view)));
                if (key === "_parent") return wrap(clip?.parent ?? hooks.owner(view));
                if (key === "_target") return targetPath(result);
                if (key === "_url") return clip?.bundle.source.href ?? hooks.owner(view)?.bundle.source.href;
                if (key === "_currentframe") return clip?.frame;
                if (key === "_totalframes" || key === "_framesloaded") return clip?.data.frames.length;
                if (key === "_quality") return String(view.stage?.quality ?? "high").toUpperCase();
                if (key === "_highquality") return view.stage?.quality === "low" ? 0 : 1;
                if (aliases.has(key)) {
                    const field = aliases.get(key);
                    return percentages.has(key) ? (view[field] ?? 1) * 100 : view[field];
                }
                if (has(values, key)) return values[key];
                if (clip) {
                    const layer = clip.namedLayer(key);
                    if (layer) return wrap(layer.child, layer.view);
                }
                if (methods.has(key)) return methods.get(key);
                let method;
                if (key === "getBytesLoaded" || key === "getBytesTotal") method = () => hooks.size(clip);
                else if (key === "hitTest") method = (first, y, shape) =>
                    hooks.hitTest(view, objects.get(first)?.view, first, y, shape);
                else if (key === "duplicateMovieClip") method = (name, depth) => hooks.clone(clip, name, depth);
                else if (key === "removeMovieClip") method = () => hooks.remove(clip);
                else if (key === "getDepth") method = () => clip?.parentLayer?.row[0] ?? 0;
                else if (key === "gotoAndPlay" || key === "gotoAndStop") method = frame => go(clip, frame, key === "gotoAndPlay");
                else if (typeof view[key] === "function") method = view[key].bind(view);
                else return view[key];
                methods.set(key, method);
                return method;
            },
            set(_, key, value) {
                if (protectedKeys.has(key)) return true;
                if (key === "_quality" || key === "_highquality") {
                    if (view.stage) view.stage.quality = key === "_quality" ? value : value ? "high" : "low";
                } else if (aliases.has(key)) {
                    view[aliases.get(key)] = percentages.has(key) ? Number(value) / 100 : value;
                } else if (key === "text" && view.text !== undefined) view.text = value;
                else {
                    values[key] = value;
                    if (clip) hooks.writeVariable(clip, key, value);
                }
                return true;
            },
        });
        wrappers.set(view, result);
        objects.set(result, { clip, view });
        return result;
    }
    function targetPath(object) {
        const parts = [];
        let item = objects.get(object);
        for (let depth = 0; item?.view && depth < 64; depth++) {
            if (item.view.name) parts.push(item.view.name);
            item = objects.get(wrap(item.clip?.parent ?? hooks.owner(item.view)));
        }
        return "/" + parts.reverse().join("/");
    }
    function resolve(clip, input) {
        if (objects.has(input)) return input;
        if (!clip) return undefined;
        const path = input == null ? "" : String(input);
        if (!path) return wrap(clip);
        const absolute = path.startsWith("/");
        const tokens = path.includes("/") ? path.split("/") : path.split(".");
        let value = wrap(absolute ? getRoot(clip) : clip);
        for (const token of tokens)
            if (token && token !== ".") value = member(value, token === ".." ? "_parent" : token);
        return value;
    }
    function member(object, key) {
        if (object == null || protectedKeys.has(String(key))) return undefined;
        return object[key];
    }
    const toClip = (clip, input) => objects.get(resolve(clip, input))?.clip;
    function variableAddress(clip, input) {
        const path = String(input);
        const colon = path.lastIndexOf(":");
        const separator = colon >= 0 ? colon : path.lastIndexOf(".");
        return separator < 0 ? [wrap(clip), path] :
            [resolve(clip, path.slice(0, separator)), path.slice(separator + 1)];
    }
    function position(clip, frame) {
        if (typeof frame === "string" && frame.includes(":")) {
            const separator = frame.lastIndexOf(":");
            clip = toClip(clip, frame.slice(0, separator));
            frame = frame.slice(separator + 1);
        }
        const number = clip?.data.labels[frame] ?? Number(frame);
        return [clip, Number.isFinite(number) ? Math.trunc(number) : 0];
    }
    function go(clip, frame, play, bias = 0) {
        const [target, value] = position(clip, frame);
        if (!target || value <= 0) return;
        if (play === undefined) target.seek(value + bias);
        else target[play ? "gotoAndPlay" : "gotoAndStop"](value + bias);
    }
    function argumentsFrom(stack) {
        const count = Number(stack.pop()) | 0;
        if (count < 0 || count > 4096) throw new RangeError("Nombre d’arguments AS1/AS2 invalide.");
        const result = new Array(count);
        for (let i = 0; i < count; i++) result[i] = stack.pop();
        return result;
    }
    function context(clip, parent = null, initialThis = wrap(clip)) {
        let target = clip, steps = 20000;
        const locals = Object.create(null);
        const registers = [];
        const global = globalScope(clip);
        const thisValue = initialThis;
        const scope = key => has(locals, key) ? locals : parent?.scope(key);
        const varAddress = input => {
            const path = String(input);
            const local = scope(path);
            return local ? [local, path] : variableAddress(target, path);
        };
        const api = {
            registers, locals, scope, constants: parent?.constants ?? [],
            step: () => {
                if (--steps <= 0) throw new Error("Boucle ActionScript 1/2 interrompue.");
                return true;
            },
            member,
            setMember(object, key, value) {
                if (object != null && !protectedKeys.has(String(key))) object[key] = value;
            },
            variable(name) {
                if (name === "this") return thisValue;
                if (name === "_global") return global;
                const [object, key] = varAddress(name);
                if (object && (has(object, key) || object !== wrap(target))) return member(object, key);
                return member(object, key) ?? global[key];
            },
            assign(name, value) { const [object, key] = varAddress(name); api.setMember(object, key, value); },
            define(name, value) { api.setMember(parent ? locals : wrap(target), name, value); },
            removeVariable(name) { return api.removeMember(...varAddress(name)); },
            removeMember(object, name) { return object != null && !protectedKeys.has(String(name)) && delete object[name]; },
            property(object, index) { return member(resolve(target, object), properties[index]); },
            setProperty(object, index, value) { api.setMember(resolve(target, object), properties[index], value); },
            setTarget(path) { target = toClip(path === "" ? clip : target, path); },
            timeline(method, frame) { return method === "seek" ? go(target, frame) : target?.[method](frame); },
            toggleQuality() { if (target) target.view.stage.quality = target.view.stage.quality === "low" ? "high" : "low"; },
            frameAvailable(frame) { const [object, index] = position(target, frame); return index > 0 && index <= object?.data.frames.length; },
            go(frame, play, bias) { go(target, frame, play, bias); },
            callFrame(frame) {
                const [object, index] = position(target, frame);
                return object?.run(object.data.frames[index - 1]?.[3]);
            },
            callFunction(name, stack, construct) {
                const fn = api.variable(name), args = argumentsFrom(stack);
                if (typeof fn === "function") return construct ? Reflect.construct(fn, args) : fn.apply(wrap(target), args);
            },
            callMethod(object, key, stack, construct) {
                const direct = key == null || key === "";
                const fn = direct ? object : member(object, key), args = argumentsFrom(stack);
                if (typeof fn === "function") return construct ? Reflect.construct(fn, args) : fn.apply(direct ? wrap(target) : object, args);
            },
            arguments: argumentsFrom,
            object(stack) {
                const count = Number(stack.pop()) | 0;
                const result = Object.create(null);
                if (count < 0 || count > 4096) throw new RangeError("Taille d’objet AS1/AS2 invalide.");
                for (let i = 0; i < count; i++) { const value = stack.pop(); api.setMember(result, stack.pop(), value); }
                return result;
            },
            function(parameters, flags, extended, body) {
                const captured = target ?? clip;
                const constants = api.constants;
                return function(...args) {
                    if (callDepth >= 128) return undefined;
                    ++callDepth;
                    try {
                        const receiver = this ?? wrap(captured);
                        const local = context(captured, api, receiver);
                        local.constants = constants;
                        let register = 1;
                        if (!extended || !(flags & 8)) local.locals.arguments = args;
                        if (extended) {
                            const preloads = [[1, receiver], [4, args], [16, undefined], [64, wrap(getRoot(captured))],
                                [128, wrap(captured.parent)], [256, global]];
                            for (const [mask, value] of preloads) if (flags & mask) local.registers[register++] = value;
                        }
                        parameters.forEach(([name, reg], index) => {
                            if (reg) local.registers[reg] = args[index];
                            else local.locals[name] = args[index];
                        });
                        return body(local);
                    } finally { --callDepth; }
                };
            },
            extend(type, base) {
                if (typeof type === "function" && typeof base === "function") type.prototype = Object.create(base.prototype);
            },
            path(value) { return objects.has(value) ? targetPath(value) : undefined; },
            typeOf(value) { return objects.get(value)?.clip ? "movieclip" : value === null ? "null" : typeof value; },
            clone(value, name, depth) { return hooks.clone(toClip(target, value), name, depth); },
            remove(value) { hooks.remove(toClip(target, value)); },
            navigate(url, destination) { hooks.navigate(clip, url, destination); },
        };
        return api;
    }
    return {
        context, wrap,
        binding(clip, path) {
            const [object, name] = variableAddress(clip, path);
            return { owner: objects.get(object)?.clip, name };
        },
        key(code, down) { lastKey = code; if (down) keys.add(code); else keys.delete(code); },
        clearKeys() { keys.clear(); }
    };
}
)JS";
}
