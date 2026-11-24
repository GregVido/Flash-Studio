import assert from "node:assert/strict";
import { EditorState } from "../src/FlashStudio/Editor/vendor/codemirror.js";
import { language, makeCatalog, suggest } from "../src/FlashStudio/Editor/as3-language.js";
import platformSymbols from "../src/FlashStudio/Editor/platform-symbols.js";
import { codeLink } from "../src/FlashStudio/Editor/code-navigation.js";

const reference = 'var target:String = "../data/external.swf?rev=42";';
const target = reference.indexOf("external");
assert.deepEqual(codeLink(reference, target, true), { action: "file", path: "../data/external.swf?rev=42" });
assert.equal(codeLink(reference, target, false), undefined);
assert.equal(codeLink("// " + reference, target + 3, true), undefined);
assert.deepEqual(codeLink("Client.send(message);", 8, false),
    { action: "reference", kind: "call", receiver: "Client", name: "send" });
assert.deepEqual(codeLink("var conn:net.Client;", 15, true),
    { action: "reference", kind: "type", name: "net.Client" });

const catalog = makeCatalog({
    "demo.Probe": [{ label: "sample", detail: "public function sample():void", type: "method", isPublic: true }],
    "demo.Packet": [],
    "net.ByteArray": []
}, platformSymbols);

function setup(text) {
    const pos = text.indexOf("|");
    const doc = text.slice(0, pos) + text.slice(pos + 1);
    const state = EditorState.create({ doc, extensions: [language(catalog)] });
    return { state, result: suggest(catalog, "demo.Probe")({ state, pos, explicit: true }) };
}

function complete(text, label, detail) {
    const { state, result } = setup(text);
    const option = result.options.find(item => item.label === label && item.detail === detail);
    assert.ok(option, `Missing suggestion ${label} ${detail}`);
    let transaction;
    option.apply({ state, dispatch(spec) { transaction = state.update(spec); } }, option, result.from, result.to);
    assert.ok(transaction);
    return transaction.newDoc.toString();
}

const body = "    public class Probe { public var bytes:ByteAr|; }";
const opened = "package demo {\n";
assert.equal(complete(opened + body + "\n}", "ByteArray", "flash.utils"),
    opened + "    import flash.utils.ByteArray;\n\n" + body.replace("ByteAr|", "ByteArray") + "\n}");
assert.equal(complete(opened + "    import flash.utils.*;\n" + body + "\n}", "ByteArray", "flash.utils"),
    opened + "    import flash.utils.*;\n" + body.replace("ByteAr|", "ByteArray") + "\n}");
assert.equal(complete(opened + "    import net.ByteArray;\n" + body + "\n}", "ByteArray", "flash.utils"),
    opened + "    import net.ByteArray;\n" + body.replace("ByteAr|", "flash.utils.ByteArray") + "\n}");
assert.equal(complete(opened + "    import net.Other; // autre\n" + body + "\n}", "ByteArray", "flash.utils"),
    opened + "    import net.Other; // autre\n    import flash.utils.ByteArray;\n" + body.replace("ByteAr|", "ByteArray") + "\n}");
assert.equal(complete(opened + "var p:Pac|ket;\n}", "Packet", "demo"), opened + "var p:Packet;\n}");
assert.ok(setup("package demo { class Probe { function f(input:Packet):void { inp| } } }")
    .result.options.some(option => option.label === "input" && option.detail === "Packet"));
assert.ok(setup("package demo { class Probe { function f():void { this.sam| } } }")
    .result.options.some(option => option.label === "sample"));
const scopes = setup("package demo { class Probe { function f(other:Packet):void {} " +
    "function g(own:Packet):void { var closure:Function = function(inner:int):void { var innerLocal:int; }; | } } }");
assert.ok(scopes.result.options.some(option => option.label === "own"));
assert.ok(!scopes.result.options.some(option => ["other", "inner", "innerLocal"].includes(option.label)));
assert.equal(setup("package demo {\n// ByteAr |\n}").result, null);
assert.equal(setup('package demo {\nvar value:String = "ByteAr|').result, null);
console.log("OK — suggestions AS3, membres et imports automatiques");
