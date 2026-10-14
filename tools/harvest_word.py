#!/usr/bin/env python3
"""Harvest Word's built-in definitions from Word-authored files in the corpus:
built-in styles (most common definition per styleId, Word 2013+ files), the
latent style table, a theme, a font table and the blank-document parts.
Writes assets/word/*.xml. Run by hand; output is committed."""
import collections
import glob
import os
import re
import sys
import zipfile

from lxml import etree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "word")
W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
NS = {"w": W}


def word16(z):
    try:
        app = z.read("docProps/app.xml").decode("utf8", "replace")
    except KeyError:
        return False
    return "<AppVersion>16." in app or "<AppVersion>15." in app


def office2013_theme(z):
    """Default Office theme of Word 2013-2022 (Calibri, accent1 4472C4)."""
    try:
        th = z.read("word/theme/theme1.xml").decode("utf8", "replace")
    except KeyError:
        return False
    return 'name="Office Theme"' in th and "4472C4" in th and "Calibri Light" in th


def strip_rsid(el):
    for e in el.iter():
        for a in list(e.attrib):
            if "rsid" in a:
                del e.attrib[a]
    for r in el.findall(".//w:rsid", NS):
        r.getparent().remove(r)
    return el


def expand_table_families(files, styles):
    """Word's 2013+ table styles (Grid Table 1 Light .. List Table 7 Colorful,
    Plain Table 1-5) come in accent variants that differ only in the accent
    theme color. Take one Word-written instance per family (including the
    renamed copies Word makes, e.g. "Grid Table 2 - Accent 11") and emit all
    variants with Word's ids (GridTable4-Accent1) and names."""
    fam_re = re.compile(r"^((?:Grid|List) Table [1-7](?: Light| Dark| Colorful)?|Plain Table [1-5])(?: - | )?(?:Accent ([1-6]))?\d*$")
    found = {}  # family -> (accent, xml)
    for f in files:
        try:
            z = zipfile.ZipFile(f)
            if not word16(z):
                continue
            s = etree.fromstring(z.read("word/styles.xml"))
        except Exception:
            continue
        for st in s.findall("w:style", NS):
            if st.get("{%s}type" % W) != "table":
                continue
            name = st.find("w:name", NS)
            if name is None:
                continue
            m = fam_re.match(name.get("{%s}val" % W) or "")
            if not m:
                continue
            fam, acc = m.group(1), m.group(2)
            if fam.startswith("Plain") and acc:
                continue
            # prefer an exact built-in id over a renamed copy
            exact = st.get("{%s}customStyle" % W) not in ("1", "true")
            key = (fam, acc or "")
            if key not in found or (exact and not found[key][0]):
                found[key] = (exact, etree.tostring(strip_rsid(st), encoding="unicode"))
    families = sorted(set(k[0] for k in found))
    made = 0
    for fam in families:
        variants = [""] + ([] if fam.startswith("Plain") else [str(i) for i in range(1, 7)])
        # a template whose accent we know (an accented one), else the plain one
        tmpl_key = next(((fam, a) for a in ["1", "2", "3", "4", "5", "6"] if (fam, a) in found), (fam, ""))
        if tmpl_key not in found:
            continue
        src_acc = tmpl_key[1]
        tmpl = found[tmpl_key][1]
        for acc in variants:
            if (fam, acc) in found and found[(fam, acc)][0]:
                x = found[(fam, acc)][1]
            else:
                if bool(acc) != bool(src_acc):
                    # the un-accented version uses text1 where accents use accentN;
                    # only expand between accented variants
                    if (fam, acc) in found:
                        x = found[(fam, acc)][1]
                    else:
                        continue
                else:
                    x = tmpl.replace('"accent%s"' % src_acc, '"accent%s"' % acc)
                    x = recolor(x, src_acc, acc)
            sid = fam.replace(" ", "") + ("-Accent" + acc if acc else "")
            nm = fam + (" Accent " + acc if acc else "")
            x = re.sub(r'w:styleId="[^"]*"', 'w:styleId="%s"' % sid, x, count=1)
            x = re.sub(r'<w:name w:val="[^"]*"/>', '<w:name w:val="%s"/>' % nm, x, count=1)
            x = re.sub(r' w:customStyle="[^"]*"', "", x)
            x = re.sub(r"<w:basedOn [^>]*/>", '<w:basedOn w:val="TableNormal"/>', x)
            styles[sid] = collections.Counter({x: 1000})
            made += 1
    print("table style families:", len(families), "styles:", made)


# Office 2013-2022 theme accents (the blank template's theme)
ACCENTS = {"1": "4472C4", "2": "ED7D31", "3": "A5A5A5", "4": "FFC000", "5": "5B9BD5", "6": "70AD47"}


def recolor(x, src, dst):
    """Recompute the cached RGB values next to theme colors for the new accent.
    Word stores both; readers use the theme reference, the RGB is a fallback.
    Values are derived by mapping each distinct source color to the same
    position in the destination accent's tint/shade ramp."""
    def tinted(hexc, tint=None, shade=None):
        r, g, b = [int(hexc[i:i + 2], 16) / 255 for i in (0, 2, 4)]
        import colorsys
        h, l, s = colorsys.rgb_to_hls(r, g, b)
        if tint is not None:
            l = l * tint + (1 - tint)
        if shade is not None:
            l = l * shade
        return "".join("%02X" % int(v * 255 + 0.5) for v in colorsys.hls_to_rgb(h, l, s))

    def fix(m):
        e = m.group(0)
        if 'accent%s"' % dst not in e:
            return e
        t = re.search(r'w:theme(?:Fill)?Tint="([0-9A-F]{2})"', e)
        sh = re.search(r'w:theme(?:Fill)?Shade="([0-9A-F]{2})"', e)
        base = ACCENTS[dst]
        val = tinted(base, int(t.group(1), 16) / 255 if t else None, int(sh.group(1), 16) / 255 if sh else None) if (t or sh) else base
        e = re.sub(r'(w:(?:color|fill))="[0-9A-F]{6}"', lambda mm: '%s="%s"' % (mm.group(1), val), e)
        return e
    return re.sub(r"<w:[A-Za-z]+ [^>]*>", fix, x)


def customized(sid, x):
    """Signs that a built-in was edited by the user: literal colors without a
    theme reference, a language override, or bold/italic on Word 2013+
    headings (which use color and size, not bold)."""
    for m in re.finditer(r"<w:color [^>]*>", x):
        if "themeColor" not in m.group(0) and 'w:val="auto"' not in m.group(0):
            return True
    if "<w:lang " in x and sid not in ("Normal",):
        return True
    if re.match(r"Heading[1-3]$", sid) and ("<w:i/>" in x or "<w:i />" in x):
        return True
    return False


def main():
    files = sorted(glob.glob(os.path.join(ROOT, "tests/corpus/oxpt/**/*.docx"), recursive=True))
    styles = collections.defaultdict(collections.Counter)
    latent = collections.Counter()
    for f in files:
        try:
            z = zipfile.ZipFile(f)
            if not word16(z):
                continue
            s = etree.fromstring(z.read("word/styles.xml"))
            raw_styles = z.read("word/styles.xml").decode("utf8", "replace")
        except Exception:
            continue
        for st in s.findall("w:style", NS):
            sid = st.get("{%s}styleId" % W)
            name = st.find("w:name", NS)
            link = st.find("w:link", NS)
            # Word writes the linked character styles of built-ins ("Heading 1
            # Char") as custom styles; keep those too.
            linked_char = (st.get("{%s}type" % W) == "character" and sid and sid.endswith("Char")
                           and link is not None and name is not None
                           and (name.get("{%s}val" % W) or "").endswith(" Char"))
            if st.get("{%s}customStyle" % W) in ("1", "true", "on") and not linked_char:
                continue
            if linked_char:
                if re.fullmatch(r"[A-Za-z0-9]+Char", sid):
                    x = etree.tostring(strip_rsid(st), encoding="unicode")
                    styles[sid][x] += 1
                continue
            if name is None or not re.fullmatch(r"[A-Za-z0-9 -]+", name.get("{%s}val" % W) or ""):
                continue
            if not re.fullmatch(r"[A-Za-z0-9-]+", sid or ""):
                continue
            # built-in ids are the English name without spaces, first letter up
            special = {"annotationtext": "commenttext", "annotationreference": "commentreference",
                       "annotationsubject": "commentsubject", "normaltable": "tablenormal",
                       "macro": "macrotext", "tableofauthorities": "tableofauthorities"}
            nm = name.get("{%s}val" % W).replace(" ", "").lower()
            nm = special.get(nm, nm)
            # "Grid Table 4 Accent 1" -> GridTable4-Accent1
            nm_dash = re.sub(r"accent(\d)$", r"-accent\1", nm)
            if sid.lower() not in (nm, nm_dash):
                continue
            x = etree.tostring(strip_rsid(st), encoding="unicode")
            if "pPrChange" in x or "rPrChange" in x:
                continue  # a tracked edit of the style, not the definition
            if "<w:numId " in x:
                continue  # tied to this document's numbering part
            blank_template = office2013_theme(z) and 'w:after="160" w:line="259"' in raw_styles
            weight = 100 if blank_template else 1
            if blank_template and customized(sid, x):
                weight = 0  # a user edit of the built-in
            if not weight:
                continue
            # gallery styles (qFormat) are shown, not hidden, in Word's default set
            if st.find("w:qFormat", NS) is not None and st.find("w:semiHidden", NS) is None:
                weight *= 2
            styles[sid][x] += weight
        ls = s.find("w:latentStyles", NS)
        if ls is not None:
            latent[etree.tostring(ls, encoding="unicode")] += 1

    os.makedirs(OUT, exist_ok=True)
    expand_table_families(files, styles)
    # a linked Char style only belongs if its paragraph style is a built-in
    for sid in list(styles):
        if sid.endswith("Char") and sid[:-4] not in styles:
            del styles[sid]
    # prefer a definition whose w:link matches the built-in
    out = ['<w:styles xmlns:w="%s" xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml">' % W]
    for sid in sorted(styles):
        best, n = styles[sid].most_common(1)[0]
        best = re.sub(r' xmlns:\w+="[^"]*"', "", best)
        out.append(best)
    out.append("</w:styles>")
    open(os.path.join(OUT, "builtin_styles.xml"), "w").write("\n".join(out) + "\n")
    lat = max(latent, key=lambda x: (x.count("lsdException"), latent[x]))
    lat = re.sub(r' xmlns:\w+="[^"]*"', "", lat)
    open(os.path.join(OUT, "latent_styles.xml"), "w").write(
        lat.replace("<w:latentStyles", '<w:latentStyles xmlns:w="%s"' % W, 1) + "\n")
    print("styles:", len(styles), "latent variants:", len(latent))


if __name__ == "__main__":
    sys.exit(main())
