"""Generate small SWFs and exercise the native CLI with Python's standard library."""

import argparse
import lzma
from pathlib import Path
import struct
import subprocess
import tempfile
import wave
import xml.etree.ElementTree as etree
import zlib


def bits(*fields):
    value = "".join(format(number & ((1 << size) - 1), f"0{size}b") for number, size in fields)
    value += "0" * (-len(value) % 8)
    return int(value, 2).to_bytes(len(value) // 8, "big")


def rectangle(right, bottom):
    return bits((15, 5), (0, 15), (right, 15), (0, 15), (bottom, 15))


def tag(code, payload=b""):
    key = (code << 6) | min(len(payload), 63)
    header = struct.pack("<H", key)
    if len(payload) >= 63:
        header += struct.pack("<I", len(payload))
    return header + payload


def shape(character=10, gradient=False, texture=None):
    records = [(1, 4), (0, 4), (2, 6), (1, 1)]
    for vertical, move in [(0, 880), (1, 880), (0, -880), (1, -880)]:
        records.extend([(1, 1), (1, 1), (9, 4), (0, 1), (vertical, 1), (move, 11)])
    records.append((0, 6))
    payload = struct.pack("<H", character) + rectangle(880, 880)
    if texture is not None:
        payload += b"\x01\x41" + struct.pack("<H", texture) + b"\x00\x00"
    elif gradient:
        payload += bytes([1, 0x10, 0, 2, 0, 255, 186, 114, 255, 64, 128, 255, 0])
    else:
        payload += bytes([1, 0, 255, 186, 114, 0])
    payload += bits(*records)
    return tag(2, payload)


def demo_body():
    avm1_push = b"\x00Bonjour, Flash Studio !\x00"
    avm1 = bytes([0x96]) + struct.pack("<H", len(avm1_push)) + avm1_push + b"\x26\x00"

    abc = struct.pack("<HH", 16, 46)
    abc += b"\x00\x00\x00\x02\x04demo\x00\x00\x00"
    abc += b"\x01\x00\x00\x01\x00\x00\x00\x00"
    abc += b"\x01\x00\x01\x01\x00\x00\x03\x24\x2a\x48\x00\x00"
    sprite = struct.pack("<HH", 7, 1) + tag(87, struct.pack("<HI", 8, 0) + b"contenu") + tag(0)
    bitmap = struct.pack("<HBHH", 9, 5, 2, 1) + zlib.compress(bytes([255, 240, 100, 60, 128, 0, 0, 128]))
    pcm = struct.pack("<HBI", 11, 0x32, 2) + b"\x83\x13\x00\x00"

    return (
        rectangle(12800, 7200) + struct.pack("<HH", 24 * 256, 1) +
        tag(69, b"\x08\x00\x00\x00") + tag(9, b"\x19\x1c\x22") +
        shape() + tag(12, avm1) + tag(39, sprite) + tag(36, bitmap) +
        tag(82, b"\x00\x00\x00\x00Demo\x00" + abc) + tag(14, pcm) +
        tag(77, b"<metadata>Flash Studio</metadata>\x00") +
        shape(character=17, texture=9) +
        tag(26, struct.pack("<BHHB", 6, 1, 10, 0)) + tag(26, struct.pack("<BHHB", 6, 2, 9, 0)) +
        tag(26, struct.pack("<BHHB", 6, 3, 17, 0)) + tag(1) + tag(0)
    )


def swf(signature, body, stored):
    return signature + bytes([15]) + struct.pack("<I", len(body) + 8) + stored


def animated_sprite(rate=2 * 256):
    frames = tag(26, struct.pack("<BHHB", 6, 1, 10, 0)) + tag(1)
    for index in range(1, 12):
        matrix = bits((0, 1), (0, 1), (13, 5), (index * 320, 13), (0, 13))
        frames += tag(26, struct.pack("<BH", 5, 1) + matrix) + tag(1)
    sprite = tag(39, struct.pack("<HH", 7, 12) + frames + tag(0))
    stage = tag(26, struct.pack("<BHHB", 6, 1, 7, 0)) + tag(1) + tag(0)
    body = rectangle(12800, 7200) + struct.pack("<HH", rate, 1) + shape() + sprite + stage
    return swf(b"FWS", body, body)


def check(exe, folder, asc=None):
    folder.mkdir(parents=True, exist_ok=True)

    def run(*args, ok=True):
        result = subprocess.run([exe, *args], capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
        return result.stdout

    def recompile(text, name):
        if not asc:
            return
        sandbox = folder / name
        sandbox.mkdir(exist_ok=True)
        source_file = sandbox / "Probe.as"
        source_file.write_text(text, encoding="utf-8")
        binary = sandbox / "Probe.abc"
        binary.unlink(missing_ok=True)
        parent = sandbox / "Base.as"
        parent.write_text("package library { public class Base {} }\n", encoding="utf-8")
        dependency = sandbox / "Base.abc"
        dependency.unlink(missing_ok=True)
        compile_as = ["java", "-jar", asc / "asc.jar", "-AS3", "-strict", "-import", asc / "builtin.abc"]
        setup = subprocess.run([*compile_as, parent.name], cwd=sandbox, capture_output=True, timeout=30)
        assert setup.returncode == 0 and dependency.is_file(), (setup.stdout, setup.stderr)
        result = subprocess.run([*compile_as, "-import", dependency.name, source_file.name],
                                cwd=sandbox, capture_output=True, timeout=30)
        assert result.returncode == 0 and binary.is_file(), (result.stdout, result.stderr)

    body = demo_body()
    plain = swf(b"FWS", body, body)
    lzma_file = lzma.compress(body, format=lzma.FORMAT_ALONE)
    compressed = zlib.compress(body)
    cases = {
        "Démo": plain,
        "zlib": swf(b"CWS", body, compressed),
        "LZMA": swf(b"ZWS", body, struct.pack("<I", len(lzma_file) - 13) + lzma_file[:5] + lzma_file[13:]),
        "LZMA-props": swf(b"ZWS", body, struct.pack("<I", len(lzma_file) - 8) + lzma_file[:5] + lzma_file[13:]),
        "zlib-stocké": swf(b"CWS", body, zlib.compress(body, level=0)),
    }
    unpacked = folder / "normalisé.swf"
    for name, content in cases.items():
        path = folder / f"{name}.swf"
        path.write_bytes(content)
        assert "640 × 360" in run("info", path)
        tags = run("list", path)
        assert "5\t87\tDefineBinaryData\t13\t4" in tags
        run("unpack", path, unpacked)
        assert unpacked.read_bytes() == plain

    path = folder / "Démo.swf"
    animation = folder / "Sprite de test.swf"
    animation.write_bytes(animated_sprite())
    first_frame = folder / "sprite-01.svg"
    last_frame = folder / "sprite-12.svg"
    run("sprite-frame", animation, "1", "1", first_frame)
    run("sprite-frame", animation, "1", "12", last_frame)
    assert 'matrix(1 0 0 1 0 0)' in first_frame.read_text(encoding="utf-8")
    assert 'matrix(1 0 0 1 3520 0)' in last_frame.read_text(encoding="utf-8")
    run("sprite-frame", animation, "1", "13", last_frame, ok=False)

    fractional = folder / "Sprite cadence fractionnaire.swf"
    fractional.write_bytes(animated_sprite(6138))
    anim_png = folder / "sprite.apng"
    anim_gif = folder / "sprite.gif"
    anim_svg = folder / "sprite animé.svg"
    for filename in (anim_png, anim_gif, anim_svg):
        run("sprite-animation", fractional, "1", filename)
    run("sprite-animation", fractional, "0", anim_png, ok=False)

    png = anim_png.read_bytes()
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    png_chunks = []
    offset = 8
    while offset < len(png):
        length, key = struct.unpack_from(">I4s", png, offset)
        payload = png[offset + 8:offset + 8 + length]
        assert zlib.crc32(key + payload) == struct.unpack_from(">I", png, offset + 8 + length)[0]
        png_chunks.append((key, payload))
        offset += length + 12
    assert png_chunks[1] == (b"acTL", struct.pack(">II", 12, 0))
    controls = [struct.unpack(">IIIIIHHBB", payload) for key, payload in png_chunks if key == b"fcTL"]
    assert len(controls) == 12
    assert all(control[1:] == (220, 44, 0, 0, 128, 3069, 0, 0) for control in controls)
    images = [zlib.decompress(payload if key == b"IDAT" else payload[4:])
              for key, payload in png_chunks if key in (b"IDAT", b"fdAT")]
    assert len(images) == 12 and (images[0][4], images[-1][4]) == (255, 0)
    assert (images[0][176 * 4 + 4], images[-1][176 * 4 + 4]) == (0, 255)

    gif = anim_gif.read_bytes()
    assert gif[:10] == b"GIF89a" + struct.pack("<HH", 220, 44) and b"NETSCAPE2.0" in gif
    offset = 13 + (3 * (2 << (gif[10] & 7)) if gif[10] & 128 else 0)
    delays = []
    frames = 0
    while gif[offset] != 0x3B:
        if gif[offset] == 0x21:
            if gif[offset + 1] == 0xF9:
                delays.append(struct.unpack_from("<H", gif, offset + 4)[0])
                assert gif[offset + 3] & 1  # Transparent color is enabled.
            offset += 2
        else:
            assert gif[offset] == 0x2C
            flags = gif[offset + 9]
            offset += 11 + (3 * (2 << (flags & 7)) if flags & 128 else 0)
            frames += 1
        while gif[offset]:
            offset += gif[offset] + 1
        offset += 1
    assert frames == len(delays) == 12 and sum(delays) == 50
    root = etree.parse(anim_svg).getroot()
    svg_ns = "{http://www.w3.org/2000/svg}"
    motion = list(root.iter(svg_ns + "animate"))
    assert len(motion) == 12 and abs(float(motion[0].get("dur")[:-1]) - (12 * 256 / 6138)) < 1e-10
    assert any("3520" in value for group in root.iter(svg_ns + "g") for value in group.attrib.values())
    oversized_apng = folder / "sprite-400.apng"
    oversized_svg = folder / "forme-400.svg"
    run("sprite-animation", animation, "1", oversized_apng, "4")
    assert struct.unpack_from(">II", oversized_apng.read_bytes(), 16) == (880, 176)
    run("image", path, "2", oversized_svg, "4")
    scaled_shape = etree.parse(oversized_svg).getroot()
    assert scaled_shape.get("width") == "176" and scaled_shape.get("height") == "176"
    assert "DefineSprite" in run("inspect", path, "4")
    assert 'trace("Bonjour, Flash Studio !");' in run("decompile", path, "3")
    assert "0000  Push" in run("disasm", path, "3")
    assert "return 42;" in run("decompile", path, "8")
    assert "pushbyte  42" in run("disasm", path, "8")
    assert "function demo():*" in run("abc-list", path, "8")
    assert "1 strictement identiques" in run("abc-audit", path, "8")
    assert '3\tActionScript\tDoAction\t3\t8\ttrace("Bonjour' in run("search", path, "BONJOUR", "--source")
    assert not run("search", path, "BONJOUR", "--source", "--case")
    assert "8\tInstructions\t" in run("search", path, r"pushbyte\s+42", "--pcode", "--regex")
    assert "10\tTextes et métadonnées\tMetadata\t" in run("search", path, "mETADATA", "--text")
    assert "8\tConstantes ABC\tConstante #1\t1\t1\tdemo" in run("search", path, "DEMO", "--constants")
    assert "2\tTags et ressources\tDefineShape #10\t" in run("search", path, "DefineShape", "--tags")
    run("search", path, "[", "--regex", ok=False)

    def push_avm1(payload):
        return b"\x96" + struct.pack("<H", len(payload)) + payload

    array_values = b"".join(b"\x07" + struct.pack("<i", number) for number in (2, 1, 2))
    actions = push_avm1(array_values) + b"\x42\x26" + push_avm1(b"\x00depart\x00") + b"\x9f\x01\x00\x01\x00"
    buttons = folder / "Boutons AVM1.swf"
    button_header = rectangle(12800, 7200) + struct.pack("<HH", 24 * 256, 1)
    original = struct.pack("<HBHHB", 10, 1, 11, 1, 0) + b"\x00" + actions
    modern = struct.pack("<HBHB", 12, 0, 3, 0) + b"\x00\x00\x04\x00" + actions
    button_body = button_header + tag(7, original) + tag(34, modern) + tag(1) + tag(0)
    buttons.write_bytes(swf(b"FWS", button_body, button_body))
    for index in ("0", "1"):
        button_source = run("decompile", buttons, index)
        assert 'trace([1, 2]);' in button_source and 'gotoAndPlay("depart");' in button_source
        assert "GotoFrame2" in run("disasm", buttons, index)
    assert "press" in run("decompile", buttons, "1")
    assert "DefineButton2" in run("search", buttons, "depart", "--source")

    tag_movie = folder / "Champs et visibilité.swf"
    fields = (rectangle(12800, 7200) + struct.pack("<HH", 24 * 256, 2) + shape() +
              tag(86, b"\x01\x00Intro\x00\x01\x01Fin\x00") +
              tag(70, struct.pack("<BBHHBB", 6, 32, 1, 10, 0, 0)) + tag(1) +
              tag(94, struct.pack("<BBHB", 1, 32, 1, 1)) + tag(1) + tag(0))
    tag_movie.write_bytes(swf(b"FWS", fields, fields))
    assert "Image 2 · Fin" in run("inspect", tag_movie, "1")
    assert "Profondeur : 1" in run("inspect", tag_movie, "2")
    run("frame", tag_movie, "1", first_frame)
    run("frame", tag_movie, "2", last_frame)
    assert "<path" not in first_frame.read_text(encoding="utf-8")
    assert "<path" in last_frame.read_text(encoding="utf-8")

    class_abc = struct.pack("<HH", 16, 46) + bytes([2, 7, 0, 0, 11])  # int pool includes the value 7
    for item in (b"demo", b"Probe", b"Base", b"value", b"int", b"read", b"library", b"XML", b"flag", b"counter"):
        class_abc += bytes([len(item)]) + item
    class_abc += bytes([4, 0x16, 1, 0x16, 0, 0x16, 7, 0, 9])  # Namespaces and multinames
    for namespace, name in ((1, 2), (3, 3), (2, 4), (2, 5), (2, 6), (2, 8), (2, 9), (2, 10)):
        class_abc += bytes([7, namespace, name])
    class_abc += bytes([3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0])  # Three method signatures
    class_abc += bytes([0, 1, 1, 2, 1, 0, 0, 2])  # Metadata and the instance header
    class_abc += bytes([3, 0, 1, 4, 0, 5, 2, 0, 2, 1])  # Instance traits and static initializer
    static_traits = len(class_abc)
    class_abc += bytes([0, 0, 3])  # Static traits, scripts and bodies
    for method_id, code in ((0, b"\x47"), (1, b"\x47"), (2, b"\x24\x2a\x48")):
        class_abc += bytes([method_id, 1, 1, 0, 0, len(code)]) + code + b"\x00\x00"
    class_header = rectangle(12800, 7200) + struct.pack("<HH", 24 * 256, 1)
    if asc:
        empty_as3 = folder / "Empty AS3.swf"
        empty_body = class_header + tag(69, b"\x08\x00\x00\x00") + tag(1) + tag(0)
        empty_as3.write_bytes(swf(b"CWS", empty_body, zlib.compress(empty_body)))
        source = folder / "Panel.as"
        panel_source = """package demo {
    public class Panel {
        public function Panel(label:String = "") {}
        public function value():String { return "first"; }
        public function make():* {
            var result:* = new Panel("copy");
            return result;
        }
    }
}"""
        source.write_text(panel_source, encoding="utf-8")
        created = folder / "New AS3 class.swf"
        run("class-new", empty_as3, "demo.Panel", source, created, asc / "asc.jar")
        assert created.read_bytes()[:3] == b"CWS"
        assert "0\tdemo.Panel" in run("abc-classes", created, "1")
        created_source = run("abc-class", created, "1", "0")
        assert 'return "first";' in created_source
        assert 'var local1:Panel = new Panel("copy");' in created_source, created_source
        rejected = folder / "Rejected AS3 class.swf"
        run("class-new", created, "demo.Panel", source, rejected, asc / "asc.jar", ok=False)
        assert not rejected.exists()

        source.write_text(panel_source.replace('"first"', '"second"'), encoding="utf-8")
        edited = folder / "Updated AS3 class.swf"
        run("class-edit", created, "1", "demo.Panel", source, edited, asc / "asc.jar")
        assert 'return "second";' in run("abc-class", edited, "1", "0")
        assert 'return "first";' in run("abc-class", created, "1", "0")
        def value_method(film):
            return next(line.split("\t", 1)[0] for line in run("abc-list", film, "1").splitlines()
                        if "\tdemo.Panel\tfunction value():String\t" in line)

        assert value_method(created) == value_method(edited)
        source.write_text("package demo { public class Panel { @@@ } }", encoding="utf-8")
        run("class-edit", created, "1", "demo.Panel", source, rejected, asc / "asc.jar", ok=False)
        assert not rejected.exists()

        decoder = folder / "Decoder.as"
        decoder.write_text("""package messages {
    public class Decoder {
        public static function reveal(first:int, second:int):String {
            return String.fromCharCode(first, second);
        }
    }
}""", encoding="utf-8")
        with_decoder = folder / "Décodeur de chaînes.swf"
        run("class-new", empty_as3, "messages.Decoder", decoder, with_decoder, asc / "asc.jar")
        caller = folder / "StringCaller.as"
        caller.write_text("""package demo {
    import messages.Decoder;
    public class StringCaller {
        public function text():String { return Decoder.reveal(79, 75); }
        public function dynamicText(code:int):String { return Decoder.reveal(code, 75); }
        public function nested():Function {
            return function():String { return Decoder.reveal(72, 73); };
        }
    }
}""", encoding="utf-8")
        with_calls = folder / "Appels au décodeur.swf"
        run("class-new", with_decoder, "demo.StringCaller", caller, with_calls, asc / "asc.jar")
        original = with_calls.read_bytes()
        cleaned_calls = folder / "Chaînes résolues.swf"
        assert "Decoder.reveal" in run("abc-class", with_calls, "1", "0")
        assert "2 appels remplacés" in run("deobfuscate", with_calls, cleaned_calls, "1", "0", "--strings")
        assert with_calls.read_bytes() == original
        resolved = run("abc-class", cleaned_calls, "1", "0")
        assert 'return "OK";' in resolved and 'return "HI";' in resolved and "return Decoder.reveal(" in resolved
        assert "0 en échec" in run("abc-audit", cleaned_calls, "1")

        source.write_text("""package demo {
    public class Probe {
        public function make():Function {
            var captured:int = 42;
            return function():int { return captured; };
        }
    }
}""", encoding="utf-8")
        closures = folder / "Fonction imbriquée.swf"
        run("class-new", empty_as3, "demo.Probe", source, closures, asc / "asc.jar")
        nested = run("abc-class", closures, "1", "0")
        assert "function():int" in nested and "return captured;" in nested and "Corps AVM2 non reconstruit" not in nested
        recompile(nested, "asc-fermeture")

    abc_header = b"\x00\x00\x00\x00Probe\x00"
    class_suffix = tag(76, struct.pack("<HH", 1, 0) + b"demo.Probe\x00") + tag(1) + tag(0)
    class_body = class_header + tag(82, abc_header + class_abc) + class_suffix
    class_movie = folder / "Classes.swf"
    class_movie.write_bytes(swf(b"FWS", class_body, class_body))
    assert "Classe du document : demo.Probe" in run("info", class_movie)
    assert "0\tdemo.Probe\t4" in run("abc-classes", class_movie, "0")
    class_source = run("abc-class", class_movie, "0", "0")
    for declaration in ("package demo", "import library.Base;", "public class Probe extends Base", "public var value:int;",
                        "public function Probe()", "public function get read():int", "return 42;"):
        assert declaration in class_source
    assert "Initialiseur statique" not in class_source
    assert "0\tActionScript\tdemo.Probe\t" in run("search", class_movie, "get read", "--source")

    loop_pcode = folder / "boucle.pcode"
    loop_pcode.write_text(""".stack 2
.locals 3
.scope 0 0
pushfalse
pushtrue
swap
jump stores
pushbyte 99
pop
pushnull
pop
stores: setlocal1
setlocal2
getlocal1
dup
iftrue fake
pop
getlocal2
fake: iffalse dead
jump payload
exit: label
returnvalue
dead: label
pushbyte 7
returnvalue
payload: pushbyte 42
jump exit
""", encoding="utf-8")
    obfuscated_movie = folder / "Flux obfusqué.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, obfuscated_movie)
    original = obfuscated_movie.read_bytes()
    clean_movie = folder / "Flux désobfusqué.swf"
    assert run("obfuscation-info", class_movie).strip() == "0"
    assert run("obfuscation-info", obfuscated_movie).strip() == "1"
    assert "1 méthodes nettoyées" in run("deobfuscate", obfuscated_movie, clean_movie, "0", "0")
    assert obfuscated_movie.read_bytes() == original
    assert run("obfuscation-info", clean_movie).strip() == "0"
    clean_source = run("abc-class", clean_movie, "0", "0")
    assert "return 42;" in clean_source and "return 7;" not in clean_source
    assert "0 en échec" in run("abc-audit", clean_movie, "0")
    recompile(clean_source, "asc-desobfuscation")

    closure = folder / "Fausse fermeture.swf"
    closure_clean = folder / "Fausse fermeture nettoyée.swf"
    closure_pcode = folder / "fermeture.pcode"
    closure_pcode.write_text(""".stack 2
.locals 1
.scope 0 1
getlocal0
pushscope
newfunction 2
pop
jump code
pushbyte 99
pop
code: pushbyte 42
returnvalue
""", encoding="utf-8")
    run("abc-edit", class_movie, "0", "2", closure_pcode, closure)
    assert run("obfuscation-info", closure).strip() == "1"
    assert "return 42;" in run("abc-class", closure, "0", "0")
    run("deobfuscate", closure, closure_clean)
    assert run("obfuscation-info", closure_clean).strip() == "0"
    closure_result = run("abc-pcode", closure_clean, "0", "2")
    assert "newfunction" not in closure_result and "pushbyte 99" not in closure_result
    assert "return 42;" in run("abc-class", closure_clean, "0", "0")

    undefined_local = folder / "Branchement sur un registre vierge.swf"
    closure_pcode.write_text(""".stack 1
.locals 2
.scope 0 1
getlocal0
pushscope
getlocal1
not
iftrue live
pushbyte 99
returnvalue
live: pushbyte 42
returnvalue
""", encoding="utf-8")
    run("abc-edit", class_movie, "0", "2", closure_pcode, undefined_local)
    local_source = run("abc-class", undefined_local, "0", "0")
    assert "return 42;" in local_source and "return 99;" not in local_source

    dynamic_calls = class_abc.replace(b"\x07counter", b"\x07counter\x08toString", 1)
    dynamic_calls = dynamic_calls.replace(b"\x0b\x04demo", b"\x0c\x04demo", 1)
    old_namespaces = bytes([4, 0x16, 1, 0x16, 0, 0x16, 7, 0, 9])
    new_namespaces = bytes([4, 0x16, 1, 0x16, 0, 0x16, 7, 2, 1, 2, 10])
    dynamic_calls = dynamic_calls.replace(old_namespaces, new_namespaces, 1)
    last_qname = dynamic_calls.index(b"\x07\x02\x0a") + 3
    dynamic_calls = dynamic_calls[:last_qname] + bytes([0x1B, 1]) + dynamic_calls[last_qname:]
    dynamic_input = folder / "Appel dynamique.swf"
    dynamic_body = class_header + tag(82, abc_header + dynamic_calls) + class_suffix
    dynamic_input.write_bytes(swf(b"FWS", dynamic_body, dynamic_body))
    indirect_call = """.stack 2
.locals 2
.scope 0 1
getlocal0
pushscope
getlocal0
dup
setlocal1
pushstring 11
getproperty 9
getlocal1
call 0
pop
kill 1
pushbyte 42
returnvalue
"""
    for code, output, compact in (
        (indirect_call, "Appel compact.swf", True),
        (indirect_call.replace("kill 1\n", "kill 1\ngetlocal1\npop\n"), "Registre relu après l’appel.swf", False),
    ):
        closure_pcode.write_text(code, encoding="utf-8")
        destination = folder / output
        run("abc-edit", dynamic_input, "0", "2", closure_pcode, destination)
        direct_source = run("abc-class", destination, "0", "0")
        assert ("this.toString();" in direct_source) == compact
        assert (".call(local1)" in direct_source) != compact

    long_class = b"\x01\x04\x05" * 36
    long_literal = b"a text constant which should remain the same " * 2 + b"and is deliberately long"
    obfuscated_names = class_abc.replace(b"\x05Probe", bytes([len(long_class)]) + long_class, 1)
    obfuscated_names = obfuscated_names.replace(b"\x05value", b"\x06_e_-_-", 1)
    obfuscated_names = obfuscated_names.replace(
        b"\x07counter", b"\x07Class_1" + bytes([len(long_literal)]) + long_literal, 1)
    obfuscated_names = obfuscated_names.replace(b"\x0b\x04demo", b"\x0c\x04demo", 1)
    old_name = b"demo." + long_class + b"\0"
    symbols = tag(76, struct.pack("<HH", 1, 0) + old_name)
    placement = tag(70, struct.pack("<BBH", 0, 8, 1) + old_name)
    name_body = class_header + tag(82, abc_header + obfuscated_names) + symbols + placement + tag(1) + tag(0)
    name_movie, renamed = folder / "Noms obfusqués.swf", folder / "Noms nettoyés.swf"
    name_original = swf(b"CWS", name_body, zlib.compress(name_body))
    name_movie.write_bytes(name_original)
    assert run("obfuscation-info", name_movie).strip() == "2"
    assert "2 identifiants renommés" in run("deobfuscate", name_movie, renamed, "0", "0", "--names")
    assert run("obfuscation-info", renamed).strip() == "0"
    assert "Classe du document : demo.Class_2" in run("info", renamed)
    assert "demo.Class_2" in run("inspect", renamed, "2")
    renamed_source = run("abc-class", renamed, "0", "0")
    assert "class Class_2" in renamed_source and "var symbol_1:int;" in renamed_source
    assert long_literal in zlib.decompress(renamed.read_bytes()[8:])
    assert name_movie.read_bytes() == name_original

    scrambled_code = loop_pcode.read_text(encoding="utf-8").replace(
        "payload: pushbyte 42\njump exit",
        "payload: getlocal0\niffalse alternative\npushbyte 42\njump exit\n"
        "alternative: pushbyte 21\njump exit")
    loop_pcode.write_text(scrambled_code, encoding="utf-8")
    scrambled_movie = folder / "Branches déplacées.swf"
    reordered = folder / "Branches réordonnées.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, scrambled_movie)
    run("deobfuscate", scrambled_movie, reordered)
    assert "0 en échec" in run("abc-audit", reordered, "0")
    reordered_pcode = run("abc-pcode", reordered, "0", "2")
    assert reordered_pcode.index("iffalse") < reordered_pcode.index("returnvalue")
    assert "pushbyte 42" in reordered_pcode and "pushbyte 21" in reordered_pcode

    static_abc = class_abc[:static_traits] + bytes([1, 7, 6, 1, 4, 0]) + class_abc[static_traits + 1:]
    static_body = class_header + tag(82, abc_header + static_abc) + class_suffix
    static_movie = folder / "Classe statique.swf"
    static_movie.write_bytes(swf(b"FWS", static_body, static_body))
    loop_pcode.write_text(""".stack 2
.locals 1
.scope 0 1
getlocal0
pushscope
getlocal0
pushbyte 42
initproperty 7
returnvoid
""", encoding="utf-8")
    initialized = folder / "Initialiseur statique.swf"
    run("abc-edit", static_movie, "0", "1", loop_pcode, initialized)
    initialized_source = run("abc-class", initialized, "0", "0")
    assert "public static const flag:int = 42;" in initialized_source, initialized_source
    recompile(initialized_source, "asc-initialiseur-statique")

    defaults_abc = class_abc[:static_traits] + bytes([
        2, 8, 0, 1, 4, 1, 3, 7, 0, 2, 4, 0
    ]) + class_abc[static_traits + 1:]
    defaults_body = class_header + tag(82, abc_header + defaults_abc) + class_suffix
    defaults_movie = folder / "Valeur statique déclarée.swf"
    defaults_movie.write_bytes(swf(b"FWS", defaults_body, defaults_body))
    static_pcode = """.stack 2
.locals 1
.scope 0 1
getlocal0
pushscope
findproperty 7
pushbyte 42
setproperty 7
findproperty 8
pushbyte 7
setproperty 8
returnvoid
"""
    loop_pcode.write_text(static_pcode, encoding="utf-8")
    repeated = folder / "Valeur statique répétée.swf"
    run("abc-edit", defaults_movie, "0", "1", loop_pcode, repeated)
    static_source = run("abc-class", repeated, "0", "0")
    assert static_source.index("public static var flag:int = 42;") < static_source.index(
        "public static var counter:int = 7;") < static_source.index("public function Probe()")
    assert "\n        flag = " not in static_source and "\n        counter = " not in static_source
    recompile(static_source, "asc-valeurs-statiques")

    loop_pcode.write_text(static_pcode.replace("pushbyte 7\n", "pushbyte 8\n"), encoding="utf-8")
    different = folder / "Valeur statique différente.swf"
    run("abc-edit", defaults_movie, "0", "1", loop_pcode, different)
    different_source = run("abc-class", different, "0", "0")
    assert "public static var counter:int = 7;" in different_source and "\n        counter = 8;" in different_source

    loop_pcode.write_text(""".stack 2
.locals 2
.scope 0 0
pushbyte 19
dup
setlocal1
pushbyte 23
add_i
returnvalue
""", encoding="utf-8")
    duplicate = folder / "Duplication et inférence.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, duplicate)
    duplicate_source = run("abc-class", duplicate, "0", "0")
    assert "var local1:int = 19;" in duplicate_source and "return local1 + 23;" in duplicate_source
    recompile(duplicate_source, "asc-duplication")

    loop_pcode.write_text(""".stack 3
.locals 2
.scope 0 0
pushbyte 1
pushbyte 2
pushbyte 3
multiply_i
add_i
convert_d
convert_i
setlocal1
pushbyte 12
pushbyte 7
getlocal1
subtract_i
subtract_i
pushbyte 2
subtract_i
returnvalue
""", encoding="utf-8")
    precedence_movie = folder / "Priorité des opérateurs.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, precedence_movie)
    precedence_source = run("abc-class", precedence_movie, "0", "0")
    assert "var local1:int = int(Number(1 + 2 * 3));" in precedence_source, precedence_source
    assert "return 12 - (7 - local1) - 2;" in precedence_source, precedence_source
    recompile(precedence_source, "asc-priorite-operateurs")

    loop_pcode.write_text(""".stack 2
.locals 2
.scope 0 0
pushbyte 0
setlocal1
jump test
boucle: inclocal_i 1
test: getlocal1
pushbyte 3
iflt boucle
getlocal1
returnvalue
""", encoding="utf-8")
    loop_movie = folder / "Boucle.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, loop_movie)
    reconstructed = run("abc-class", loop_movie, "0", "0")
    assert "var local1:* = 0;" in reconstructed
    assert "while (local1 < 3)\n" in reconstructed
    assert "return local1;" in reconstructed
    assert "3 corps reconstruits" in run("abc-coverage", loop_movie, "0")
    assert 'label="iflt"' in run("abc-graph", loop_movie, "0", "2")
    recompile(reconstructed, "asc-boucle")
    class_project = folder / "projet-classes"
    run("export-project", loop_movie, class_project)
    assert (class_project / "as3" / "demo" / "Probe.as").read_text(encoding="utf-8") == reconstructed

    loop_pcode.write_text(""".stack 2
.locals 2
.scope 0 0
pushbyte 0
setlocal1
jump test
boucle: getlocal1
pushbyte 2
ifne suite
jump sortie
suite: inclocal_i 1
test: getlocal1
pushbyte 3
iflt boucle
sortie: getlocal1
returnvalue
""", encoding="utf-8")
    break_movie = folder / "Sortie de boucle.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, break_movie)
    break_source = run("abc-class", break_movie, "0", "0")
    assert "break;" in break_source and "return local1;" in break_source, break_source
    recompile(break_source, "asc-sortie-boucle")

    activation_movie = folder / "Activation.swf"
    activation_abc = class_abc[:-1] + bytes([1, 3, 0, 1, 4, 0])
    activation_body = class_header + tag(82, abc_header + activation_abc) + class_suffix
    activation_movie.write_bytes(swf(b"FWS", activation_body, activation_body))
    loop_pcode.write_text(""".stack 2
.locals 2
.scope 0 2
getlocal0
pushscope
newactivation
dup
setlocal1
pushscope
getscopeobject 1
pushbyte 42
setslot 1
getlocal1
getslot 1
returnvalue
""", encoding="utf-8")
    edited_activation = folder / "Activation éditée.swf"
    run("abc-edit", activation_movie, "0", "2", loop_pcode, edited_activation)
    activation_source = run("abc-class", edited_activation, "0", "0")
    assert "var value:int;" in activation_source and "value = 42;" in activation_source, activation_source
    assert "return value;" in activation_source, activation_source
    recompile(activation_source, "asc-activation")

    no_default = folder / "Initialisation implicite d’une variable capturée.swf"
    activation_code = loop_pcode.read_text(encoding="utf-8").replace(
        "getscopeobject 1\npushbyte 42", "getscopeobject 1\npushbyte 0\nsetslot 1\ngetscopeobject 1\npushbyte 42")
    loop_pcode.write_text(activation_code, encoding="utf-8")
    run("abc-edit", activation_movie, "0", "2", loop_pcode, no_default)
    assert "value = 0;" not in run("abc-class", no_default, "0", "0")

    shared_exit = folder / "Retour partagé.swf"
    loop_pcode.write_text(""".stack 1
.locals 1
.scope 0 1
getlocal0
pushscope
getlocal0
iffalse done
getlocal0
iffalse other
jump done
other: getlocal0
iftrue done
pushbyte 42
returnvalue
done: returnvoid
""", encoding="utf-8")
    run("abc-edit", class_movie, "0", "1", loop_pcode, shared_exit)
    shared_exit_source = run("abc-source", shared_exit, "0", "1")
    assert shared_exit_source.count("if (") == 3 and "return 42;" in shared_exit_source, shared_exit_source
    assert "Reconstruction indisponible" not in shared_exit_source, shared_exit_source

    loop_pcode.write_text(""".stack 1
.locals 1
.scope 0 0
pushbyte 1
lookupswitch defaut premier second
premier: pushbyte 2
returnvalue
second: pushbyte 3
returnvalue
defaut: pushbyte 0
returnvalue
""", encoding="utf-8")
    switch_movie = folder / "Switch.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, switch_movie)
    switch_source = run("abc-class", switch_movie, "0", "0")
    assert "switch (int(1))" in switch_source and "case 1:" in switch_source and "default:" in switch_source
    assert "Corps AVM2 non reconstruit" not in switch_source
    recompile(switch_source, "asc-switch")

    loop_pcode.write_text(""".stack 2
.locals 2
.scope 0 0
jump tests
premier: pushbyte 14
returnvalue
second: pushbyte 20
returnvalue
defaut: pushbyte 0
returnvalue
tests: pushstring 1
setlocal1
pushstring 1
getlocal1
ifstrictne autre
pushbyte 0
jump repartition
autre: pushstring 2
getlocal1
ifstrictne manque
pushbyte 1
jump repartition
manque: jump index_defaut
pushbyte 2
jump repartition
index_defaut: pushbyte 2
repartition: kill 1
lookupswitch defaut premier second defaut
""", encoding="utf-8")
    string_switch = folder / "Switch chaînes.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, string_switch)
    switch_source = run("abc-class", string_switch, "0", "0")
    assert 'switch ("demo")' in switch_source and 'case "demo":' in switch_source and 'case "Probe":' in switch_source, switch_source
    recompile(switch_source, "asc-switch-chaines")

    loop_pcode.write_text(""".stack 2
.locals 1
.scope 0 0
dxns 1
findpropstrict 6
pushstring 2
constructprop 6 1
getdescendants 3
convert_i
returnvalue
""", encoding="utf-8")
    xml_movie = folder / "E4X.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, xml_movie)
    xml_source = run("abc-class", xml_movie, "0", "0")
    assert 'default xml namespace = "demo";' in xml_source and '..value' in xml_source, xml_source
    recompile(xml_source, "asc-e4x")

    for opcode, statement in (("nextname", "for (local3 in local2)"), ("nextvalue", "for each (local3 in local2)")):
        loop_pcode.write_text(f""".stack 2
.locals 4
.scope 0 0
pushbyte 0
setlocal1
pushbyte 42
newarray 1
setlocal2
jump test
corps: label
getlocal2
getlocal1
{opcode}
coerce_a
setlocal3
test: hasnext2 2 1
iftrue corps
kill 2
kill 1
pushbyte 42
returnvalue
""", encoding="utf-8")
        enumeration = folder / f"{opcode}.swf"
        run("abc-edit", class_movie, "0", "2", loop_pcode, enumeration)
        source = run("abc-class", enumeration, "0", "0")
        assert statement in source and "Corps AVM2 non reconstruit" not in source
        recompile(source, f"asc-{opcode}")

    typed_enumeration = loop_pcode.read_text(encoding="utf-8").replace(
        "coerce_a\nsetlocal3", "coerce 4\ndup\nsetlocal3\nconvert_i\nsetlocal3")
    loop_pcode.write_text(typed_enumeration, encoding="utf-8")
    typed_movie = folder / "Énumération typée.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, typed_movie)
    typed_source = run("abc-class", typed_movie, "0", "0")
    assert "for each (var item:* in local2)" in typed_source and "local3 = int(item);" in typed_source, typed_source
    recompile(typed_source, "asc-enumeration-typee")

    loop_pcode.write_text(""".stack 3
.locals 3
.scope 0 3
.exception debut fin gestion 0 0
getlocal0
pushscope
debut: pushbyte 4
setlocal1
fin: jump suite
gestion: getlocal0
pushscope
newcatch 0
dup
setlocal2
dup
pushscope
swap
setslot 1
getlocal2
getslot 1
convert_i
setlocal1
popscope
kill 2
suite: getlocal1
returnvalue
""", encoding="utf-8")
    catch_movie = folder / "Exceptions.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, catch_movie)
    catch_source = run("abc-class", catch_movie, "0", "0")
    assert "try\n" in catch_source and "catch (error:*)" in catch_source and "int(error)" in catch_source
    recompile(catch_source, "asc-exceptions")

    catch_code = loop_pcode.read_text(encoding="utf-8")
    region = catch_code[catch_code.index("debut:"):catch_code.index("suite:")].replace("gestion:", "rescue:")
    second_region = (region.replace("debut", "next_start").replace("fin", "next_end")
                     .replace("rescue", "next_rescue").replace("suite", "complete").replace("newcatch 0", "newcatch 1"))
    two_catches = catch_code[:catch_code.index("debut:")].replace(
        ".exception debut fin gestion 0 0", ".exception debut fin rescue 0 0\n.exception next_start next_end next_rescue 0 0")
    two_catches += region + "suite: nop\n" + second_region + "complete: getlocal1\nreturnvalue\n"
    loop_pcode.write_text(two_catches, encoding="utf-8")
    twice = folder / "Exceptions successives.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, twice)
    both_blocks = run("abc-class", twice, "0", "0")
    assert both_blocks.count("catch (error:*)") == 2
    recompile(both_blocks, "asc-exceptions-successives")

    opaque_code = catch_code.replace(".locals 3", ".locals 5").replace(
        "getlocal0\npushscope\ndebut: pushbyte 4",
        "pushfalse\npushtrue\nswap\njump stores\npushbyte 99\npop\npushnull\npop\n"
        "stores: setlocal3\nsetlocal 4\ngetlocal0\npushscope\ndebut: getlocal3\n"
        "iftrue parasite\npushbyte 4\njump affecter\nparasite: pushbyte 7\naffecter: nop", 1)
    loop_pcode.write_text(opaque_code, encoding="utf-8")
    opaque_catch = folder / "Exceptions obfusquées.swf"
    cleaned_catch = folder / "Exceptions désobfusquées.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, opaque_catch)
    assert run("obfuscation-info", opaque_catch).strip() == "1"
    run("deobfuscate", opaque_catch, cleaned_catch)
    clean_catch_source = run("abc-class", cleaned_catch, "0", "0")
    assert "try\n" in clean_catch_source and "catch (error:*)" in clean_catch_source
    assert "0 en échec" in run("abc-audit", cleaned_catch, "0")
    recompile(clean_catch_source, "asc-desobfuscation-exceptions")

    guarded_code = catch_code.replace(
        "pushscope\ndebut:", "pushscope\npushbyte 0\nsetlocal1\ngetlocal1\niffalse suite\ndebut:", 1)
    loop_pcode.write_text(guarded_code, encoding="utf-8")
    guarded_movie = folder / "Exceptions dans une condition.swf"
    run("abc-edit", class_movie, "0", "2", loop_pcode, guarded_movie)
    guarded_source = run("abc-class", guarded_movie, "0", "0")
    assert "if (local1)\n" in guarded_source and "catch (error:*)" in guarded_source, guarded_source
    recompile(guarded_source, "asc-exceptions-conditionnelles")

    loop_pcode.write_text(""".stack 1
.locals 1
.scope 0 0
jump relais
pushbyte 99
returnvalue
relais: jump resultat
resultat: pushbyte 42
returnvalue
""", encoding="utf-8")
    run("abc-edit", path, "8", "0", loop_pcode, loop_movie)
    simplified = run("abc-simplify", loop_movie, "8", "0")
    assert "Instructions inaccessibles retirées (3) · destinations redirigées (1)" in simplified
    assert "pushbyte 99" not in simplified
    loop_pcode.write_text(simplified, encoding="utf-8")
    cleaned = folder / "Nettoyé.swf"
    run("abc-edit", loop_movie, "8", "0", loop_pcode, cleaned)
    assert "return 42;" in run("abc-source", cleaned, "8", "0")

    loop_pcode.write_text(""".stack 1
.locals 1
.scope 0 0
pushtrue
iftrue vivant
pushbyte 99
returnvalue
vivant: pushbyte 42
returnvalue
""", encoding="utf-8")
    run("abc-edit", path, "8", "0", loop_pcode, loop_movie)
    simplified = run("abc-simplify", loop_movie, "8", "0")
    assert "Branchements à booléen constant simplifiés (1)" in simplified
    assert "pushbyte 99" not in simplified and "iftrue" not in simplified

    method = folder / "méthode.pcode"
    edited_abc = folder / "abc-modifié.swf"
    source = run("abc-pcode", path, "8", "0")
    for line_ending in ("\n", "\r\n", "\r"):
        method.write_bytes(source.replace("\n", line_ending).encode("utf-8"))
        run("abc-edit", path, "8", "0", method, edited_abc)
        assert edited_abc.read_bytes() == plain
    method.write_text(""".stack 1
.locals 1
.scope 0 0
debut:
    pushtrue
    iffalse resultat
    pushshort 300
    returnvalue
resultat:
    pushbyte 5
    returnvalue
""", encoding="utf-8")
    run("abc-edit", path, "8", "0", method, edited_abc)
    assert "return 300;" in run("abc-source", edited_abc, "8", "0")
    assert "if (true)" in run("abc-source", edited_abc, "8", "0")
    assert "iffalse L0009" in run("abc-pcode", edited_abc, "8", "0")
    invalid = folder / "refusé.swf"
    method.write_text(method.read_text(encoding="utf-8").replace("iffalse resultat", "iffalse absent"), encoding="utf-8")
    run("abc-edit", path, "8", "0", method, invalid, ok=False)
    assert not invalid.exists()
    method.write_text(""".stack 1
.locals 1
.scope 0 0
.exception debut fin gestion 0 0
debut: pushbyte 0
    lookupswitch defaut premier
premier: pushshort 301
    returnvalue
defaut: pushbyte -5
    returnvalue
fin:
gestion: pop
    returnvoid
""", encoding="utf-8")
    run("abc-edit", path, "8", "0", method, edited_abc)
    view = run("abc-pcode", edited_abc, "8", "0")
    assert "lookupswitch L" in view and ".exception L0000 L" in view
    method.write_text(view, encoding="utf-8")
    roundtrip = folder / "abc-aller-retour.swf"
    run("abc-edit", edited_abc, "8", "0", method, roundtrip)
    assert roundtrip.read_bytes() == edited_abc.read_bytes()
    method.write_text(""".stack 1
.locals 1
.scope 0 0
.anchor interieur numero 2
numero: pushshort -240
    returnvalue
    jump interieur
    jump fin
fin:
""", encoding="utf-8")
    run("abc-edit", path, "8", "0", method, edited_abc)
    patched = run("abc-pcode", edited_abc, "8", "0")
    assert ".anchor L0002 L0000 2" in patched
    assert "pushshort 4294967056  ; -240" in patched
    method.write_text(patched, encoding="utf-8")
    run("abc-edit", edited_abc, "8", "0", method, roundtrip)
    assert roundtrip.read_bytes() == edited_abc.read_bytes()
    method.write_text("nouveauNom", encoding="utf-8")
    run("abc-string", path, "8", "1", method, edited_abc)
    assert "function nouveauNom():*" in run("abc-list", edited_abc, "8")
    exported_abc = folder / "démo.abc"
    run("export", path, "8", exported_abc)
    run("abc-replace", path, "8", exported_abc, roundtrip)
    assert roundtrip.read_bytes() == plain
    packed = folder / "compressé.swf"
    run("compress", path, packed)
    assert packed.read_bytes().startswith(b"CWS")
    run("unpack", packed, unpacked)
    assert unpacked.read_bytes() == plain
    xml = folder / "document.xml"
    run("export-xml", path, xml)
    assert etree.parse(xml).getroot().attrib["version"] == "15"
    run("import-xml", xml, unpacked)
    assert unpacked.read_bytes() == plain
    unsafe = folder / "dtd.xml"
    unsafe.write_text('<!DOCTYPE swf [<!ENTITY x SYSTEM "file:///C:/invalid">]><swf version="15" frame-header=""/>')
    run("import-xml", unsafe, unpacked, ok=False)

    svg = folder / "trace.svg"
    run("export", path, "2", svg)
    root = etree.parse(svg).getroot()
    assert root.attrib["viewBox"] == "0 0 880 880"
    assert root[0].attrib["fill"] == "#FFBA72"
    assert root[0].attrib["d"] == "M0 0L880 0L880 880L0 880L0 0Z"
    frame = folder / "scène.svg"
    run("frame", path, "1", frame)
    stage = etree.parse(frame).getroot()
    assert stage.attrib["viewBox"] == "0 0 12800 7200"
    assert any(element.get("fill") == "#FFBA72" for element in stage.iter("{http://www.w3.org/2000/svg}path"))
    assert "data:image/png;base64," in frame.read_text(encoding="utf-8")
    shape_png = folder / "forme.png"
    run("image", path, "2", shape_png)
    encoded = shape_png.read_bytes()
    assert struct.unpack_from(">II", encoded, 16) == (44, 44)
    data = encoded.index(b"IDAT")
    size, = struct.unpack_from(">I", encoded, data - 4)
    pixels = zlib.decompress(encoded[data + 4 : data + 4 + size])
    middle = 22 * (1 + 44 * 4) + 1 + 22 * 4
    assert pixels[middle : middle + 4] == bytes([255, 186, 114, 255])
    run("image", path, "2", shape_png, "2")
    assert struct.unpack_from(">II", shape_png.read_bytes(), 16) == (88, 88)
    raster_frame = folder / "scene.png"
    run("frame", path, "1", raster_frame)
    assert struct.unpack_from(">II", raster_frame.read_bytes(), 16) == (640, 360)
    visual_batch = folder / "visuels-svg"
    run("export-visuals", path, visual_batch, ".svg")
    assert (visual_batch / "000002-DefineShape.svg").read_bytes() == svg.read_bytes()
    run("export-visuals", path, visual_batch, ".svg", ok=False)
    assert "clip-texture-17" in frame.read_text(encoding="utf-8")
    run("frame", path, "2", frame, ok=False)

    editable = folder / "textes.swf"
    edited = folder / "textes-modifiés.swf"
    new_text = folder / "texte.txt"
    new_text.write_text("Remplacé ✓", encoding="utf-8")
    edit_tag = tag(37, struct.pack("<H", 1) + rectangle(300, 200) + b"\x00\x00variable\x00")
    font = tag(48, b"\x02\x00\x04\x00\x01X\x01\x00\x04\x00\x06\x00\x00\x00A\x00")
    static = tag(11, struct.pack("<H", 3) + rectangle(300, 200) + b"\x00\x01\x01" +
                 struct.pack("<BHHBB", 0x88, 2, 240, 1, 0) + b"\x00")
    text_body = rectangle(400, 400) + struct.pack("<HH", 6144, 1) + edit_tag + font + static
    text_body += shape(character=4, gradient=True) + tag(1) + tag(0)
    editable.write_bytes(swf(b"FWS", text_body, text_body))
    run("replace-text", editable, "0", new_text, edited)
    output_text = folder / "extrait.txt"
    run("export", edited, "0", output_text)
    assert output_text.read_text(encoding="utf-8") == "Remplacé ✓"
    run("export", edited, "2", output_text)
    assert output_text.read_bytes() == b"A"
    gradient = folder / "dégradé.svg"
    run("export", edited, "3", gradient)
    assert 'fill="url(#gradient-4-1)"' in gradient.read_text(encoding="utf-8")

    png = folder / "pixels.png"
    run("export", path, "7", png)
    png_bytes = png.read_bytes()
    assert png_bytes.startswith(b"\x89PNG\r\n\x1a\n")
    assert png_bytes[16:24] == struct.pack(">II", 2, 1)
    offset = 8
    while offset < len(png_bytes):
        length = struct.unpack_from(">I", png_bytes, offset)[0]
        name = png_bytes[offset + 4:offset + 8]
        chunk = png_bytes[offset + 8:offset + 8 + length]
        crc = struct.unpack_from(">I", png_bytes, offset + 8 + length)[0]
        assert zlib.crc32(name + chunk) == crc
        if name == b"IDAT":
            assert zlib.decompress(chunk) == bytes([0, 240, 100, 60, 255, 0, 0, 255, 128])
        offset += length + 12
    for suffix, signature in [(".bmp", b"BM"), (".jpg", b"\xff\xd8"), (".tif", b"II*\x00")]:
        converted = png.with_suffix(suffix)
        run("image", path, "7", converted)
        assert converted.read_bytes().startswith(signature)

    wav = folder / "son.wav"
    run("export", path, "9", wav)
    with wave.open(str(wav), "rb") as sound:
        assert (sound.getframerate(), sound.getsampwidth(), sound.getnchannels()) == (5512, 2, 1)
        assert sound.readframes(2) == b"\x83\x13\x00\x00"

    exported = folder / "ressources"
    report = run("export-all", path, exported)
    assert "0 tags en échec" in report
    assert (exported / "000002-DefineShape.svg").read_bytes() == svg.read_bytes()
    assert (exported / "000007-DefineBitsLossless2.png").read_bytes() == png_bytes
    assert "return 42;" in (exported / "000008-DoABC2.as").read_text(encoding="utf-8")
    project = Path(tempfile.mkdtemp(prefix="projet-", dir=folder))
    assert "0 tags en échec" in run("export-project", path, project)
    assert (project / "film.swf").read_bytes() == plain
    assert "function demo():*" in (project / "methodes-abc.csv").read_text(encoding="utf-8")
    for name in ("film.xml", "scene.svg", "tags.csv"):
        assert (project / name).is_file()
    assert any((project / "scripts" / "abc-000008").glob("*.pcode"))
    run("export-project", path, project, ok=False)

    replacement = folder / "nouveau.bin"
    destination = folder / "édité.swf"
    output = folder / "exporté.bin"
    for data in [bytes(range(128)), b"", "contenu très court".encode()]:
        replacement.write_bytes(data)
        run("replace-binary", path, "5", replacement, destination)
        run("export", destination, "5", output)
        assert output.read_bytes() == data
        assert "return 42;" in run("decompile", destination, "8")
        path = destination

    malformed = folder / "invalide.swf"
    for content in [plain[:-1], swf(b"CWS", body, compressed[:-1] + bytes([compressed[-1] ^ 1])),
                    swf(b"FWS", body[:-2], body[:-2])]:
        malformed.write_bytes(content)
        run("info", malformed, ok=False)
    run("export", folder / "Démo.swf", "1000", output, ok=False)
    print("OK — FWS, CWS, ZWS, XML, ABC, P-code, scènes, images, projet, textes et entrées invalides")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--asc", type=Path, help="Dossier contenant asc.jar et builtin.abc pour vérifier la recompilation AS3")
    args = parser.parse_args()
    asc = args.asc.resolve() if args.asc else None
    if args.fixtures:
        check(args.executable.resolve(), args.fixtures.resolve(), asc)
    else:
        artifacts = Path(__file__).resolve().parent.parent / "artifacts"
        artifacts.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=artifacts) as temporary:
            check(args.executable.resolve(), Path(temporary), asc)
