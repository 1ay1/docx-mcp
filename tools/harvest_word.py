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


def strip_rsid(el):
    for e in el.iter():
        for a in list(e.attrib):
            if "rsid" in a:
                del e.attrib[a]
    for r in el.findall(".//w:rsid", NS):
        r.getparent().remove(r)
    return el


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
            if name is None or not re.fullmatch(r"[A-Za-z0-9 ]+", name.get("{%s}val" % W) or ""):
                continue
            if not re.fullmatch(r"[A-Za-z0-9]+", sid or ""):
                continue
            # built-in ids are the English name without spaces, first letter up
            special = {"annotationtext": "commenttext", "annotationreference": "commentreference",
                       "annotationsubject": "commentsubject", "normaltable": "tablenormal",
                       "macro": "macrotext", "tableofauthorities": "tableofauthorities"}
            nm = name.get("{%s}val" % W).replace(" ", "").lower()
            nm = special.get(nm, nm)
            if sid.lower() != nm:
                continue
            x = etree.tostring(strip_rsid(st), encoding="unicode")
            styles[sid][x] += 1
        ls = s.find("w:latentStyles", NS)
        if ls is not None:
            latent[etree.tostring(ls, encoding="unicode")] += 1

    os.makedirs(OUT, exist_ok=True)
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
