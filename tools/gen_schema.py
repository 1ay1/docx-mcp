#!/usr/bin/env python3
"""Generate src/schema/schema_data.cpp from the Open XML SDK schema data
(schemas/openxml-sdk). Run by hand when the data changes; the output is
committed so the build needs no Python."""
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "schemas", "openxml-sdk")
OUT = os.path.join(ROOT, "src", "schema", "schema_data.cpp")

VERSIONS = {None: 0, "Office2007": 1, "Office2010": 2, "Office2013": 3, "Office2016": 4,
            "Office2019": 5, "Office2021": 6, "Microsoft365": 7}
TARGET = 7

# value kinds, must match ValueKind in schema.hpp
VK = {"StringValue": 0, "BooleanValue": 1, "OnOffValue": 2, "TrueFalseValue": 3,
      "TrueFalseBlankValue": 4, "ByteValue": 5, "SByteValue": 6, "Int16Value": 7,
      "UInt16Value": 8, "Int32Value": 9, "UInt32Value": 10, "Int64Value": 11,
      "UInt64Value": 12, "IntegerValue": 13, "DecimalValue": 14, "DoubleValue": 15,
      "SingleValue": 16, "HexBinaryValue": 17, "Base64BinaryValue": 18,
      "DateTimeValue": 19, "EnumValue": 20, "ListValue": 21}
PK = {"Element": 0, "Sequence": 1, "Choice": 2, "Group": 3, "All": 4, "Any": 5}
ANY_NS = {"##any": 0, "##other": 1, "##local": 2, "##targetNamespace": 3}


class Strings:
    def __init__(self):
        self.idx = {}
        self.items = []

    def __call__(self, s):
        if s is None:
            s = ""
        if s not in self.idx:
            self.idx[s] = len(self.items)
            self.items.append(s)
        return self.idx[s]


def applies(v):
    """Does a versioned validator / facet apply to the Microsoft365 target?"""
    ver = v.get("Version")
    if ver is None:
        return True
    if v.get("IsInitialVersion"):
        return VERSIONS[ver] <= TARGET
    return VERSIONS[ver] == TARGET


def occurs(p):
    occ = p.get("Occurs")
    if occ is None:
        return 1, 1
    # several entries = per-version occurs; the last is the newest
    o = occ[-1]
    return o.get("Min", 0), o.get("Max", 0)


def cstr(s):
    out = ['"']
    for ch in s.encode("utf-8"):
        c = chr(ch)
        if c == '"' or c == "\\":
            out.append("\\" + c)
        elif 32 <= ch < 127:
            out.append(c)
        else:
            out.append("\\%03o" % ch)
    out.append('"')
    return "".join(out)


def main():
    S = Strings()
    S("")
    namespaces = json.load(open(os.path.join(DATA, "namespaces.json")))
    typed_ns = json.load(open(os.path.join(DATA, "typed-namespaces.json")))
    prefix_of_uri = {n["Uri"]: n["Prefix"] for n in namespaces}
    dotnet_of_prefix = {n["Prefix"]: n["Namespace"] for n in typed_ns}

    types = {}
    enums = {}  # full .NET name -> enum
    for f in sorted(glob.glob(os.path.join(DATA, "schemas", "*.json"))):
        d = json.load(open(f))
        tns = d.get("TargetNamespace", "")
        pfx = prefix_of_uri.get(tns)
        dn = dotnet_of_prefix.get(pfx, "")
        for t in d.get("Types", []):
            types[t["Name"]] = t
        for e in d.get("Enums", []):
            full = dn + "." + e["Name"]
            enums[full] = e
            enums.setdefault("ST:" + e["Type"], e)

    type_names = sorted(types)
    tindex = {n: i for i, n in enumerate(type_names)}

    # enums
    enum_list = []
    enum_index = {}
    facets = []
    for name in sorted(k for k in enums if not k.startswith("ST:")):
        e = enums[name]
        b = len(facets)
        for fc in e["Facets"]:
            if VERSIONS.get(fc.get("Version"), 0) <= TARGET:
                facets.append(S(fc["Value"]))
        enum_index[name] = len(enum_list)
        enum_index.setdefault("ST:" + e["Type"], len(enum_list))
        enum_list.append((S(name), S(e["Type"]), b, len(facets)))

    # particles
    particles = []
    pchildren = []

    def emit_particle(p):
        kind = PK.get(p.get("Kind", "Element"), 0)
        mn, mx = occurs(p)
        tid = -1
        ns = 0
        if kind == 0:
            tid = tindex[p["Name"]] if p["Name"] in tindex else -1
        if kind == 5:
            ns = ANY_NS.get(p.get("Namespace", "##any"), 0)
        idx = len(particles)
        particles.append(None)
        kids = [emit_particle(c) for c in p.get("Items", [])]
        b = len(pchildren)
        pchildren.extend(kids)
        particles[idx] = (kind, mn, mx, tid, b, len(pchildren), ns)
        return idx

    # attributes + validators
    attrs = []
    validators = []

    def num_kind(ty):
        if ty is None:
            return 0
        t = ty.lower()
        if "boolean" in t:
            return 4
        if "double" in t or "decimal" in t or "percentage" in t and "fixed" in t:
            return 3  # float
        if "unsigned" in t or "nonnegative" in t or ("twips" in t and "signed" not in t) \
                or ("hps" in t and "signed" not in t) or "drawingelementid" in t or "positive" in t:
            return 2  # non-negative integer
        return 1  # signed integer

    def emit_validators(vs):
        b = len(validators)
        for v in vs or []:
            if not applies(v):
                continue
            name = v["Name"]
            args = {a.get("Name"): a for a in v.get("Arguments", [])}
            union = v.get("UnionId", -1)
            if name == "RequiredValidator":
                req = args.get("IsRequired", {}).get("Value", "True") != "False"
                validators.append((0, union, 1 if req else 0, -1, 0, 0, 0, 0, -1, -1, -1, 0))
            elif name == "StringValidator":
                flags = 0
                for bit, key in enumerate(["IsToken", "IsNcName", "IsQName", "IsId", "IsUri"]):
                    if args.get(key, {}).get("Value") == "True":
                        flags |= 1 << bit
                if v.get("IsList"):
                    flags |= 1 << 5
                if v.get("Type") in ("w:ST_HexColorRGB", "w:ST_StylePaneSortMethods_O12"):
                    flags |= 1 << 6  # value is hexBinary; lengths count octets
                pat = S(args["Pattern"]["Value"]) if "Pattern" in args else 0
                mnl = int(args["MinLength"]["Value"]) if "MinLength" in args else -1
                mxl = int(args["MaxLength"]["Value"]) if "MaxLength" in args else -1
                ln = int(args["Length"]["Value"]) if "Length" in args else -1
                validators.append((1, union, flags, -1, 0, 0, 0, 0, mnl, mxl, ln, pat))
            elif name == "NumberValidator":
                nflags = 0
                mn = mx = 0
                if "MinInclusive" in args:
                    nflags |= 1; mn = int(args["MinInclusive"]["Value"])
                if "MinExclusive" in args:
                    nflags |= 2; mn = int(args["MinExclusive"]["Value"])
                if "MaxInclusive" in args:
                    nflags |= 4; mx = int(args["MaxInclusive"]["Value"])
                if "MaxExclusive" in args:
                    nflags |= 8; mx = int(args["MaxExclusive"]["Value"])
                if args.get("IsPositive", {}).get("Value") == "True":
                    nflags |= 2; mn = 0
                if args.get("IsNonNegative", {}).get("Value") == "True":
                    nflags |= 1; mn = 0
                if v.get("IsList"):
                    nflags |= 16
                validators.append((2, union, nflags, -1, num_kind(v.get("Type")), 0, mn, mx, -1, -1, -1, 0))
            elif name == "EnumValidator":
                ei = enum_index.get("ST:" + v.get("Type", ""), -1)
                validators.append((3, union, 0, ei, 0, 0, 0, 0, -1, -1, -1, 0))
            elif name == "OfficeVersionValidator":
                pass
        return b, len(validators)

    def value_kind(tstr):
        base = tstr.split("<")[0]
        vk = VK.get(base, 0)
        item = 0
        ei = -1
        if base == "EnumValue":
            inner = tstr[tstr.index("<") + 1:-1]
            ei = enum_index.get(inner, -1)
        if base == "ListValue":
            inner = tstr[tstr.index("<") + 1:-1]
            ib = inner.split("<")[0]
            item = VK.get(ib, 0)
            if ib == "EnumValue":
                ei = enum_index.get(inner[inner.index("<") + 1:-1], -1)
        return vk, item, ei

    def emit_attrs(t):
        b = len(attrs)
        for a in t.get("Attributes", []):
            vk, item, ei = value_kind(a["Type"])
            vb, ve = emit_validators(a.get("Validators"))
            attrs.append((S(a["QName"]), vk, item, ei, vb, ve, VERSIONS.get(a.get("Version"), 0)))
        return b, len(attrs)

    rows = []
    for n in type_names:
        t = types[n]
        key = S(n)
        qname = S(n.split("/", 1)[1]) if "/" in n else 0
        base = -1
        if t.get("IsDerived"):
            bk = n.split("/", 1)[0] + "/"
            base = tindex.get(bk, -1)
        part = emit_particle(t["Particle"]) if "Particle" in t else -1
        ab, ae = emit_attrs(t)
        tvb, tve = emit_validators(t.get("Validators"))
        flags = 0
        if t.get("IsLeafElement"): flags |= 1
        if t.get("IsLeafText"): flags |= 2
        if t.get("IsAbstract"): flags |= 4
        if t.get("IsDerived"): flags |= 8
        comp = {"OneSequence": 1, "OneChoice": 2, "OneAll": 3}.get(t.get("CompositeType"), 0)
        rows.append((key, qname, base, part, ab, ae, tvb, tve, flags, comp,
                     VERSIONS.get(t.get("Version"), 0), S(t.get("ClassName", "")),
                     S(t.get("Summary", ""))))

    # parts
    part_rows = []
    part_json = {}
    for f in sorted(glob.glob(os.path.join(DATA, "parts", "*.json"))):
        p = json.load(open(f))
        part_json[p["Name"]] = p
    for name in sorted(part_json):
        p = part_json[name]
        q = p
        while not q.get("Root") and q.get("Base") in part_json:
            q = part_json[q["Base"]]
        root = -1
        if q.get("Root"):
            for n in type_names:
                if n.startswith(q["Root"] + "/") and n.split("/", 1)[1].split(":")[-1] == (p.get("RootElement") or q.get("RootElement")):
                    root = tindex[n]
                    break
        part_rows.append((S(p["Name"]), S(p.get("ContentType")), S(p.get("RelationshipType")),
                          root, S(p.get("Target")), S(p.get("Paths", {}).get("General")),
                          S(p.get("Extension")), VERSIONS.get(p.get("Version"), 0)))

    ns_rows = [(S(n["Prefix"]), S(n["Uri"]), VERSIONS.get(n.get("Version"), 0)) for n in namespaces]

    o = []
    o.append("// GENERATED by tools/gen_schema.py from the Open XML SDK schema data. Do not edit.")
    o.append('#include "schema_data.hpp"')
    o.append("namespace docx::schema::data {")
    o.append("const char* const strings[] = {")
    for i in range(0, len(S.items), 8):
        o.append("  " + ", ".join(cstr(s) for s in S.items[i:i + 8]) + ",")
    o.append("};")
    o.append("const std::size_t n_strings = %d;" % len(S.items))

    def table(name, ty, rows_):
        o.append("const %s %s[] = {" % (ty, name))
        for r in rows_:
            o.append("  {" + ",".join(str(x) for x in r) + "},")
        if not rows_:
            o.append("  {},")
        o.append("};")
        o.append("const std::size_t n_%s = %d;" % (name, len(rows_)))

    table("types", "RawType", rows)
    table("particles", "RawParticle", particles)
    o.append("const std::int32_t particle_children[] = {")
    for i in range(0, len(pchildren), 24):
        o.append("  " + ",".join(str(x) for x in pchildren[i:i + 24]) + ",")
    o.append("};")
    table("attrs", "RawAttr", attrs)
    table("validators", "RawValidator",
          [(k, u, fl, ei, nk, 0, "%dLL" % mn, "%dLL" % mx, mnl, mxl, ln, pat)
           for (k, u, fl, ei, nk, _, mn, mx, mnl, mxl, ln, pat) in validators])
    table("enums", "RawEnum", enum_list)
    o.append("const std::uint32_t enum_facets[] = {")
    for i in range(0, len(facets), 24):
        o.append("  " + ",".join(str(x) for x in facets[i:i + 24]) + ",")
    o.append("};")
    table("parts", "RawPart", part_rows)
    table("namespaces", "RawNamespace", ns_rows)
    o.append("}  // namespace docx::schema::data")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as fh:
        fh.write("\n".join(o) + "\n")
    print("types=%d particles=%d attrs=%d validators=%d enums=%d strings=%d parts=%d" % (
        len(rows), len(particles), len(attrs), len(validators), len(enum_list), len(S.items), len(part_rows)))


if __name__ == "__main__":
    sys.exit(main())
