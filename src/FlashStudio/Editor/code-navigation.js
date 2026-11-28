const tokens = /\/\/.*|\/\*.*?\*\/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*/g;

export function sourceMemberAtLine(ranges, line) {
    return ranges?.find(([, first, end]) => line >= first && line < end)?.[0];
}

function unquote(value) {
    return value.slice(1, -1).replace(/\\(?:u([\da-f]{4})|x([\da-f]{2})|(["'\\]))/gi,
        (_, unicode, byte, escaped) => escaped ?? String.fromCharCode(parseInt(unicode ?? byte, 16)));
}

export function codeLink(line, column, control, instructions = false) {
    for (const match of line.matchAll(tokens)) {
        const token = match[0];
        const start = match.index;
        const end = start + token.length;
        if (column < start || column >= end) continue;
        if (token.startsWith("/") || !token) return undefined;
        if (token[0] === '"' || token[0] === "'") {
            if (!control) return undefined;
            const path = unquote(token).trim();
            return /\.swf(?:[?#].*)?$/i.test(path) ? { action: "file", path } : undefined;
        }
        if (instructions) return undefined;
        const before = line.slice(0, start);
        const following = line.slice(end);
        const dot = token.lastIndexOf(".");
        const receiver = dot < 0 ? "" : token.slice(0, dot);
        const name = token.slice(dot + 1);
        if (/^\s*\(/.test(following) && !/\b(?:function|function\s+get|function\s+set)\s*$/.test(before) &&
            column > start + dot) {
            const kind = /\bnew\s*$/.test(before) ? "constructor" : "call";
            if (control && kind === "constructor") return { action: "reference", kind: "type", name: token };
            return { action: "reference", kind, receiver, name };
        }
        if (control) {
            const offset = column - start;
            const prefix = token.slice(0, token.indexOf(".", offset) < 0 ? token.length : token.indexOf(".", offset));
            return { action: "reference", kind: "type", name: prefix };
        }
    }
    return undefined;
}
