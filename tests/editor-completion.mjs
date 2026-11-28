import assert from "node:assert/strict";
import { EditorState } from "../src/FlashStudio/Editor/vendor/codemirror.js";
import { language, makeCatalog, suggest } from "../src/FlashStudio/Editor/as3-language.js";
import platformSymbols from "../src/FlashStudio/Editor/platform-symbols.js";
import { codeLink, sourceMemberAtLine } from "../src/FlashStudio/Editor/code-navigation.js";
import { pcodeLanguage, suggestPcode } from "../src/FlashStudio/Editor/pcode-language.js";

const reference = 'var target:String = "../data/external.swf?rev=42";';
const target = reference.indexOf("external");
assert.deepEqual(codeLink(reference, target, true), { action: "file", path: "../data/external.swf?rev=42" });
assert.equal(codeLink(reference, target, false), undefined);
assert.equal(codeLink("// " + reference, target + 3, true), undefined);
assert.deepEqual(codeLink("Client.send(message);", 8, false),
    { action: "reference", kind: "call", receiver: "Client", name: "send" });
assert.deepEqual(codeLink("var conn:net.Client;", 15, true),
    { action: "reference", kind: "type", name: "net.Client" });
const methodRanges = [[3, 8, 14], [0, 15, 28], [6, 29, 30]];
assert.equal(sourceMemberAtLine(methodRanges, 8), 3);
assert.equal(sourceMemberAtLine(methodRanges, 13), 3);
assert.equal(sourceMemberAtLine(methodRanges, 14), undefined);
assert.equal(sourceMemberAtLine(methodRanges, 20), 0);
assert.equal(sourceMemberAtLine(methodRanges, 29), 6);
assert.equal(sourceMemberAtLine(methodRanges, 30), undefined);
assert.equal(sourceMemberAtLine(undefined, 8), undefined);

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

const pcode = suggestPcode([
    ["getlocal", 0x62, 1], ["getlocal0", 0xd0, 0], ["pushbyte", 0x24, 1], ["jump", 0x10, 1], ["lookupswitch", 0x1b, 2]
], false);
function instruction(text, explicit = true) {
    const pos = text.indexOf("|");
    const state = EditorState.create({
        doc: text.slice(0, pos) + text.slice(pos + 1),
        extensions: [pcodeLanguage]
    });
    return { state, result: pcode({ state, pos, explicit }) };
}

const opcode = instruction("L0000:  getl|oc", false);
assert.equal(opcode.state.doc.sliceString(opcode.result.from, opcode.result.to), "getloc");
assert.equal(opcode.result.options.find(option => option.label === "getlocal").apply, "getlocal ");
assert.equal(opcode.result.options.find(option => option.label === "getlocal0").apply, "getlocal0");
assert.equal(instruction("L0000:  pushb| 4").result.options[0].apply, "pushbyte");
assert.deepEqual(instruction(".lo|").result.options.map(option => option.label), [".locals"]);
assert.deepEqual(instruction(".locals 0x3\nStart: getlocal |").result.options.map(option => option.label), ["0", "1", "2"]);
assert.equal(instruction(".locals 1\nStart: pushbyte |").result, null);
assert.deepEqual(instruction("Start: jump T|\nTarget: nop\n.anchor Tail Target 1").result.options.map(option => option.label),
    ["Target", "Tail"]);
assert.deepEqual(instruction("Start: nop\n.anchor Inner Start 1\n.anchor New |").result.options.map(option => option.label),
    ["Start"]);
assert.ok(instruction(".exception Start,End,Ha|\nHandler: nop").result.options.some(option => option.label === "Handler"));
assert.ok(instruction("L0000: lookupswitch Default, C|\nCaseOne: nop").result.options.some(option => option.label === "CaseOne"));
assert.equal(instruction("L0000: nop ; ge|tlocal").result, null);
assert.equal(instruction("Lab|el: nop").result, null);
console.log("OK — suggestions P-code, directives, registres et destinations");
