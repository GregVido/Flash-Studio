#pragma once

#include <string_view>

namespace flash::web
{
inline constexpr std::string_view class_runtime = R"JS(const owns = (object, key) => Object.hasOwn(object, key);
const classNames = new WeakMap();
export const convertedClassName = value => classNames.get(value);

const nativeBases = new Set(["", "Object", "flash.events.Event", "flash.events.EventDispatcher", "flash.display.DisplayObject",
    "flash.display.InteractiveObject", "flash.display.DisplayObjectContainer", "flash.display.Sprite", "flash.display.MovieClip",
    "flash.display.SimpleButton", "flash.display.Shape", "flash.display.Loader", "flash.display.Bitmap",
    "flash.text.TextField", "flash.text.Font", "flash.net.Socket", "flash.net.XMLSocket",
    "flash.net.URLLoader", "flash.net.LocalConnection", "flash.media.Sound", "flash.utils.ByteArray", "flash.utils.Timer"]);

function defaultValue(type) {
    if (type === "*") return undefined;
    if (type === "Boolean") return false;
    if (type === "int" || type === "uint") return 0;
    if (type === "Number") return NaN;
    return null;
}

function coerce(type, value) {
    if (type === "int") return value | 0;
    if (type === "uint") return value >>> 0;
    if (type === "Number") return Number(value);
    if (type === "Boolean") return Boolean(value);
    if (type === "String") return value == null ? null : String(value);
    if (type !== "*" && value === undefined) return null;
    return value;
}

function sortOn(items, fields, options = 0) {
    if (!Array.isArray(items)) return items.sortOn(fields, options);
    const names = Array.isArray(fields) ? fields : [fields];
    const flags = Array.isArray(options) ? options : names.map(() => options);
    const compare = ([left], [right]) => {
        for (const [index, name] of names.entries()) {
            const settings = flags[index] | 0;
            let a = left?.[name], b = right?.[name];
            if (settings & 16) {
                a = Number(a); b = Number(b);
                if (Number.isNaN(a)) a = Infinity;
                if (Number.isNaN(b)) b = Infinity;
            } else {
                a = String(a); b = String(b);
                if (settings & 1) { a = a.toLowerCase(); b = b.toLowerCase(); }
            }
            const order = a < b ? -1 : a > b ? 1 : 0;
            if (order) return settings & 2 ? -order : order;
        }
        return 0;
    };
    const rows = items.map((value, index) => [value, index]).sort(compare);
    if ((flags[0] & 4) && rows.some((row, index) => index && compare(rows[index - 1], row) === 0)) return 0;
    if (flags[0] & 8) return rows.map(([, index]) => index);
    rows.forEach(([value], index) => { items[index] = value; });
    return items;
}

export class ConvertedClasses {
    #definitions;
    #hooks;
    #records = new WeakMap();
    #types = new Map();
    #parents = new Map();
    #ancestry = new Map();
    #members = new Map();
    #depth = 0;
    #activeOwner = "";

    constructor(definitions, hooks = {}) {
        this.#definitions = definitions;
        this.#hooks = hooks;
    }

    #definition(name) {
        if (!owns(this.#definitions, name)) throw new Error("Classe ActionScript indisponible : " + name);
        return this.#definitions[name];
    }

    #lineage(name) {
        if (this.#parents.has(name)) return this.#parents.get(name);
        const chain = [], seen = new Set();
        for (let current = name; owns(this.#definitions, current); current = this.#definition(current).base) {
            if (seen.has(current) || chain.length >= 128) throw new Error("Héritage ActionScript invalide : " + name);
            seen.add(current);
            chain.push(current);
        }
        this.#parents.set(name, chain);
        return chain;
    }

    #record(name, host, staticAccess) {
        this.#definition(name);
        return { name, host, staticAccess, fields: new Map(), members: new Map(), contexts: new Map(),
            constructors: new Set(), dynamic: Object.create(null), phase: "declared", view: null };
    }

    #findMember(staticAccess, start, lexical, key) {
        const members = staticAccess ? "statics" : "methods";
        const slots = staticAccess ? "staticSlots" : "slots";
        const search = owner => {
            const def = this.#definition(owner);
            const field = Object.hasOwn(def[slots], key) ? def[slots][key] : null;
            const method = Object.hasOwn(def[members], key) ? def[members][key] : null;
            const getter = def[members]["get:" + key], setter = def[members]["set:" + key];
            return { owner, field, method, getter, setter };
        };
        if (lexical) {
            const entry = search(lexical);
            if (entry.field?.hidden || entry.method?.hidden || entry.getter?.hidden || entry.setter?.hidden) return entry;
        }
        let accessor = null;
        const chain = staticAccess ? [start] : this.#lineage(start);
        for (const owner of chain) {
            const entry = search(owner);
            if (entry.field && !entry.field.hidden || entry.method && !entry.method.hidden)
                return accessor ?? entry;
            if (entry.getter && !entry.getter.hidden || entry.setter && !entry.setter.hidden) {
                accessor ??= { owner };
                if (!accessor.getter && entry.getter && !entry.getter.hidden) {
                    accessor.getter = entry.getter;
                    accessor.getterOwner = owner;
                }
                if (!accessor.setter && entry.setter && !entry.setter.hidden) {
                    accessor.setter = entry.setter;
                    accessor.setterOwner = owner;
                }
                if (accessor.getter && accessor.setter) break;
            }
        }
        return accessor;
    }

    #member(members, staticAccess, start, lexical, key) {
        if (typeof key !== "string") return null;
        const cached = members.get(key);
        if (cached !== undefined) return cached;
        const entry = this.#findMember(staticAccess, start, lexical, key);
        if (entry !== null || members.size < 256) members.set(key, entry);
        return entry;
    }

    #invoke(record, owner, method, args) {
        if (!method) return undefined;
        if (method.error) throw new Error(method.error);
        if (!method.call) throw new Error("Méthode ActionScript sans corps : " + owner);
        if (this.#depth >= 128) throw new RangeError("Profondeur des appels ActionScript dépassée.");
        const previousOwner = this.#activeOwner;
        try {
            ++this.#depth;
            this.#activeOwner = owner;
            return Reflect.apply(method.call, undefined, [this.#context(record, owner), ...args]);
        } finally {
            this.#activeOwner = previousOwner;
            --this.#depth;
        }
    }

    #proxy(record, start, lexical = "") {
        const registry = this;
        const scope = (record.staticAccess ? "s:" : "i:") + start + "\0";
        const cache = owner => {
            const key = scope + owner;
            let members = registry.#members.get(key);
            if (!members) { members = new Map(); registry.#members.set(key, members); }
            return members;
        };
        const members = cache(lexical);
        const find = key => {
            let owner = lexical;
            if (!owner && start === record.name && !record.staticAccess && registry.#activeOwner &&
                registry.#lineage(record.name).includes(registry.#activeOwner))
                owner = registry.#activeOwner;
            return registry.#member(owner === lexical ? members : cache(owner), record.staticAccess, start, owner, key);
        };
        const target = record.staticAccess ? function (value) {
            if (value == null || registry.isType(value, record.name)) return value;
            throw new TypeError("Conversion ActionScript invalide vers " + record.name);
        } : record.dynamic;
        const view = new Proxy(target, {
            construct: (_, args) => this.create(record.name, args),
            has: (target, key) => Reflect.has(target, key) || Boolean(find(key)) ||
                owns(record.dynamic, key) || (record.host != null && key in record.host),
            get: (_, key) => {
                if (key === Symbol.hasInstance && record.staticAccess) return value => registry.isType(value, record.name);
                if (typeof key !== "string") return undefined;
                const entry = find(key);
                if (entry?.getter) return this.#invoke(record, entry.getterOwner ?? entry.owner, entry.getter, []);
                if (entry?.field) {
                    const value = record.fields.get(entry.owner)?.[key];
                    return value == null && record.host && record.host[key] != null ? record.host[key] : value;
                }
                if (entry?.method) {
                    let method = record.members.get(entry.method);
                    if (!method) {
                        method = (...args) => this.#invoke(record, entry.owner, entry.method, args);
                        record.members.set(entry.method, method);
                    }
                    return method;
                }
                return owns(record.dynamic, key) ? record.dynamic[key] : record.host?.[key];
            },
            set: (_, key, value) => {
                const entry = find(key);
                if (entry?.setter) this.#invoke(record, entry.setterOwner ?? entry.owner, entry.setter, [value]);
                else if (entry?.field) record.fields.get(entry.owner)[key] = coerce(entry.field.type, value);
                else if (entry?.getter || entry?.method) throw new TypeError("Propriété ActionScript en lecture seule : " + String(key));
                else if (record.host) record.host[key] = value;
                else record.dynamic[key] = value;
                return true;
            },
        });
        classNames.set(view, record.name);
        return view;
    }

    #context(record, owner) {
        let context = record.contexts.get(owner);
        if (context) return context;
        const registry = this;
        const base = this.#definition(owner).base;
        const view = this.#proxy(record, record.name, owner);
        context = {
            view, self: record.view,
            superView: this.#proxy(record, base),
            get staticView() {
                const type = record.staticAccess ? record.name : owner;
                const target = registry.#static(type);
                return registry.#context(target, type).view;
            },
            superInit: (...args) => this.#construct(record, base, args),
            create: (name, ...args) => this.create(name, args),
            type: name => {
                const target = this.#static(name);
                return name === owner ? this.#context(target, name).view : target.view;
            },
            native: (name, ...args) => this.#hooks.native(name, ...args),
            nativeClass: name => this.#hooks.nativeClass(name),
            nativeSymbol: name => this.#hooks.nativeSymbol(name),
            sortOn,
            construct: (value, ...args) => Reflect.construct(value, args),
            dynamicType: (value, type, cast) => {
                if (typeof type !== "function") throw new TypeError("Classe ActionScript invalide.");
                const matches = value != null && value instanceof type;
                return cast ? (matches ? value : null) : matches;
            },
            cast: (value, name) => {
                if (value == null || this.isType(value, name)) return value;
                throw new TypeError("Conversion ActionScript invalide vers " + name);
            },
            string: value => value == null ? null : String(value),
            isType: (value, name) => this.isType(value, name),
            asType: (value, name) => this.isType(value, name) ? value : null,
            event: (kind, ...args) => this.#hooks.event(kind, ...args),
            eventName: name => this.#hooks.eventName(name),
        };
        record.contexts.set(owner, context);
        return context;
    }

    #defaults(record, owners) {
        for (const owner of owners) {
            const { slots, staticSlots } = this.#definition(owner);
            const declarations = record.staticAccess ? staticSlots : slots;
            const values = Object.create(null);
            for (const [key, slot] of Object.entries(declarations)) values[key] = defaultValue(slot.type);
            record.fields.set(owner, values);
        }
        for (const owner of owners) {
            const { slots, staticSlots } = this.#definition(owner);
            const declarations = record.staticAccess ? staticSlots : slots;
            for (const [key, slot] of Object.entries(declarations))
                if (slot.value) record.fields.get(owner)[key] = coerce(slot.type, slot.value(this.#context(record, owner)));
        }
    }

    #static(name) {
        const existing = this.#types.get(name);
        if (existing) {
            if (existing.phase === "failed") throw existing.error;
            return existing;
        }
        const definition = this.#definition(name);
        const record = this.#record(name, null, true);
        record.view = this.#proxy(record, name);
        this.#types.set(name, record);
        this.#records.set(record.view, record);
        try {
            this.#lineage(name);
            if (owns(this.#definitions, definition.base)) this.#static(definition.base);
            this.#defaults(record, [name]);
            this.#invoke(record, name, definition.initialize, []);
            record.phase = "initialized";
            return record;
        } catch (error) {
            record.phase = "failed";
            record.error = error;
            throw error;
        }
    }
)JS" R"JS(
    #construct(record, name, args) {
        if (!owns(this.#definitions, name)) {
            if (!nativeBases.has(name)) throw new Error("API Flash héritée à porter : " + name);
            this.#hooks.initializeBase?.(name, record.host, args);
            return;
        }
        if (record.constructors.has(name)) throw new Error("Constructeur ActionScript répété : " + name);
        record.constructors.add(name);
        const definition = this.#definition(name);
        if (definition.construct) this.#invoke(record, name, definition.construct, args);
        else this.#construct(record, definition.base, []);
    }

    attach(name, host = null) {
        const record = this.#record(name, host, false);
        record.view = this.#proxy(record, name);
        this.#records.set(record.view, record);
        return {
            view: record.view,
            initialize: (...args) => {
                if (record.phase !== "declared") return;
                record.phase = "initializing";
                this.#static(name);
                this.#defaults(record, this.#lineage(name).slice().reverse());
                this.#construct(record, name, args);
                record.phase = "initialized";
            },
        };
    }

    create(name, args = []) {
        const native = this.#hooks.host?.(this.#lineage(name), this.#definitions);
        const instance = this.attach(name, native);
        if (native) {
            native.bindView?.(instance.view);
            this.#hooks.bindNativeView?.(native, instance.view);
        }
        instance.initialize(...args);
        return instance.view;
    }

    has(name) { return owns(this.#definitions, name); }
    definition(name) { return this.#static(name).view; }

    isType(value, name) {
        if (value == null) return false;
        const record = this.#records.get(value);
        if (record && !record.staticAccess) {
            let names = this.#ancestry.get(record.name);
            if (!names) {
                names = new Set();
                const pending = [record.name];
                while (pending.length) {
                    const current = pending.pop();
                    if (names.has(current)) continue;
                    names.add(current);
                    const shortName = current.slice(current.lastIndexOf(".") + 1);
                    if (!owns(this.#definitions, shortName)) names.add(shortName);
                    if (owns(this.#definitions, current)) {
                        const { base, interfaces } = this.#definition(current);
                        pending.push(base, ...interfaces);
                    }
                }
                this.#ancestry.set(record.name, names);
            }
            if (names.has(name)) return true;
        }
        switch (name) {
        case "Object": return true;
        case "Class": return typeof value === "function";
        case "Array": return Array.isArray(value);
        case "int": return typeof value === "number" && value === (value | 0);
        case "uint": return typeof value === "number" && value === (value >>> 0);
        case "String": case "Number": case "Boolean": return typeof value === name.toLowerCase();
        case "Date": return value instanceof Date;
        case "RegExp": return value instanceof RegExp;
        case "Error": case "ArgumentError": return value instanceof Error;
        case "TypeError": return value instanceof TypeError;
        case "RangeError": return value instanceof RangeError;
        }
        return this.#hooks.isType?.(value, name) ?? false;
    }
}
)JS";
}
