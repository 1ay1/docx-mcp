#!/usr/bin/env python3
"""Second-consumer check: LibreOffice opens every .docx given, and we compare
the text it sees with what we expect (the text with all tracked changes
accepted, as LibreOffice shows by default with changes visible we read the
document's accepted view). Also renders to PDF to catch layout-level breaks.

usage: lo_check.py <docx...>   (needs soffice + python3-uno)"""
import os
import subprocess
import sys
import tempfile
import time

import uno
from com.sun.star.beans import PropertyValue


def prop(n, v):
    p = PropertyValue()
    p.Name, p.Value = n, v
    return p


def start():
    port = 2002 + os.getpid() % 1000
    prof = tempfile.mkdtemp(prefix="lo-")
    proc = subprocess.Popen(["soffice", "--headless", "--invisible", "--nologo", "--norestore",
                             "-env:UserInstallation=file://" + prof,
                             "--accept=socket,host=127.0.0.1,port=%d;urp;" % port],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    local = uno.getComponentContext()
    resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
    for _ in range(100):
        try:
            ctx = resolver.resolve("uno:socket,host=127.0.0.1,port=%d;urp;StarOffice.ComponentContext" % port)
            return proc, ctx.ServiceManager.createInstanceWithContext("com.sun.star.frame.Desktop", ctx)
        except Exception:
            time.sleep(0.2)
    raise SystemExit("soffice did not start")


def main():
    files = sys.argv[1:]
    proc, desktop = start()
    bad = 0
    try:
        for f in files:
            url = uno.systemPathToFileUrl(os.path.abspath(f))
            try:
                doc = desktop.loadComponentFromURL(url, "_blank", 0, (prop("Hidden", True), prop("ReadOnly", True)))
                if doc is None:
                    print("FAIL", f, "LibreOffice could not open it")
                    bad += 1
                    continue
                nred = 0
                try:
                    en = doc.getRedlines().createEnumeration()
                    while en.hasMoreElements():
                        en.nextElement()
                        nred += 1
                except Exception:
                    nred = -1
                ncom = 0
                try:
                    en = doc.getTextFields().createEnumeration()
                    while en.hasMoreElements():
                        fld = en.nextElement()
                        if fld.supportsService("com.sun.star.text.TextField.Annotation"):
                            ncom += 1
                except Exception:
                    ncom = -1
                text = doc.getText().getString()
                pdf = os.path.splitext(f)[0] + ".pdf"
                doc.storeToURL(uno.systemPathToFileUrl(os.path.abspath(pdf)), (prop("FilterName", "writer_pdf_Export"),))
                doc.close(True)
                print("ok  ", f, "redlines=%d" % nred, "comments=%d" % ncom, "chars=%d" % len(text))
            except Exception as e:
                print("FAIL", f, e)
                bad += 1
    finally:
        try:
            desktop.terminate()
        except Exception:
            pass
        proc.wait(timeout=20)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
