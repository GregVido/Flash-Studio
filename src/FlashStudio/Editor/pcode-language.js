import { StreamLanguage } from "./vendor/codemirror.js";

const definition = /^\s*([A-Za-z_]\w*):/;
const directive = /^\.[A-Za-z_]\w*/;
const opName = /^[A-Za-z_]\w*/;
const registerOps = new Set([0x08, 0x32, 0x62, 0x63, 0x92, 0x94, 0xc2, 0xc3]);

export const pcodeLanguage = StreamLanguage.define({
    startState: () => ({ command: true }),
    token(stream, state) {
        if (stream.sol()) state.command = true;
        if (stream.eatSpace()) return null;
        if (stream.match(/^;.*/)) return "lineComment";
        if (state.command && stream.match(definition)) return "typeName";
        if (state.command && stream.match(directive)) {
            state.command = false;
            return "atom";
        }
        if (state.command && stream.match(opName)) {
            state.command = false;
            return "keyword";
        }
        if (stream.match(/^"(?:[^"\\]|\\.)*"/)) return "string";
        if (stream.match(/^-?(?:0x[0-9a-f]+|[0-9]+)/i)) return "number";
        if (stream.match(opName)) return "typeName";
        stream.next();
        return null;
    }
});

const contextCache = new WeakMap();

function methodContext(doc) {
    const cached = contextCache.get(doc);
    if (cached) return cached;
    const labels = new Set();
    const anchors = new Set();
    let locals = 0;
    for (const line of doc.iterLines()) {
        const declared = definition.exec(line)?.[1];
        if (declared) labels.add(declared);
        const anchor = /^\s*\.anchor\s+([A-Za-z_]\w*)[\s,]/.exec(line)?.[1];
        if (anchor) anchors.add(anchor);
        const count = /^\s*\.locals\s+(0x[0-9a-f]+|\d+)(?=\s|;|$)/i.exec(line)?.[1];
        if (count) locals = Number(count);
    }
    const result = { labels, anchors, locals };
    contextCache.set(doc, result);
    return result;
}

export function suggestPcode(opcodes, french) {
    const commands = new Map(opcodes.map(([name, code, operands]) => [name, { code, operands }]));
    const directives = [
        [".stack", french ? "Profondeur de pile maximale" : "Maximum stack depth"],
        [".locals", french ? "Nombre de registres locaux" : "Local register count"],
        [".scope", french ? "Portées initiale et maximale" : "Initial and maximum scope depths"],
        [".exception", french ? "Début, fin, cible, type, variable" : "Start, end, target, type, variable"],
        [".anchor", french ? "Nom, instruction, déplacement" : "Name, instruction, offset"]
    ];
    const mnemonicOptions = [
        ...directives.map(([label, detail]) => ({ label, detail, type: "keyword", insert: label + " " })),
        ...Array.from(commands, ([label, { code, operands }]) => ({
            label,
            detail: code === 0x1b ? (french ? "Cible par défaut et cas" : "Default target and cases") :
                french ? `${operands} opérande${operands === 1 ? "" : "s"}` : `${operands} operand${operands === 1 ? "" : "s"}`,
            type: "keyword",
            insert: label + (operands ? " " : "")
        }))
    ];

    return ({ state, pos, explicit }) => {
        const line = state.doc.lineAt(pos);
        const offset = pos - line.from;
        const comment = line.text.indexOf(";");
        if (comment >= 0 && offset > comment) return null;
        const labelEnd = definition.exec(line.text)?.[0].length ?? 0;
        if (offset < labelEnd) return null;
        const before = line.text.slice(labelEnd, offset);
        const prefix = /[^\s,;:]*$/.exec(before)[0];
        if (!prefix && !explicit) return null;
        const prior = before.slice(0, before.length - prefix.length).split(/[\s,]+/).filter(Boolean);
        const suffix = /^[^\s,;:]*/.exec(line.text.slice(offset))[0];
        const from = pos - prefix.length;
        const to = pos + suffix.length;

        if (!prior.length) {
            const separated = /^[\s,]/.test(line.text.slice(to));
            return {
                from, to,
                options: mnemonicOptions.filter(item => item.label.startsWith(prefix.toLowerCase())).map(
                    ({ label, detail, type, insert }) => ({ label, detail, type, apply: separated ? label : insert }))
            };
        }

        const command = prior[0];
        const code = commands.get(command)?.code;
        const operand = prior.length - 1;
        const branch = code >= 0x0c && code <= 0x1b && (operand === 0 || code === 0x1b);
        const exception = command === ".exception" && operand < 3;
        const anchor = command === ".anchor" && operand === 1;
        if (branch || exception || anchor) {
            const { labels, anchors } = methodContext(state.doc);
            const destinations = anchor ? labels : new Set([...labels, ...anchors]);
            return {
                from, to,
                options: Array.from(destinations).filter(label => label.startsWith(prefix)).map(label =>
                    ({ label, type: "variable", detail: french ? "Étiquette" : "Label" }))
            };
        }
        if (registerOps.has(code) && operand < commands.get(command).operands && /^\d*$/.test(prefix)) {
            const { locals } = methodContext(state.doc);
            const options = [];
            for (let index = 0; index < Math.min(locals, 1024) && options.length < 50; index++) {
                const label = String(index);
                if (label.startsWith(prefix))
                    options.push({ label, type: "variable", detail: french ? "Registre local" : "Local register" });
            }
            return { from, to, options };
        }
        return null;
    };
}
