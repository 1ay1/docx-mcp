#!/usr/bin/env python3
"""Interleave the newer tools (fields, citations, charts, shapes, lists, binding,
table styles, HTML) at random on one document, with and without tracking, and
save. Any crash or internal error fails. Outputs go to WORK for oxval.

usage: chaos.py <docx-mcp> <work dir> [seed] [runs]"""
import json,subprocess,random,sys,os
exe=sys.argv[1]
work=sys.argv[2]
os.makedirs(work,exist_ok=True)
seed=int(sys.argv[3]) if len(sys.argv)>3 else random.randint(0,10**6)
runs=int(sys.argv[4]) if len(sys.argv)>4 else 40
r=random.Random(seed)
p=subprocess.Popen([exe],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
i=[0]
def call(n,**a):
    i[0]+=1
    p.stdin.write(json.dumps({"jsonrpc":"2.0","id":i[0],"method":"tools/call" if i[0]>1 else "initialize","params":{"name":n,"arguments":a} if i[0]>1 else {"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"x","version":"1"}}})+"\n");p.stdin.flush()
    line=p.stdout.readline()
    if not line: print(f"SERVER DIED on {n} {json.dumps(a)[:300]} seed={seed}"); sys.exit(2)
    res=json.loads(line).get("result",{}); t=res.get("content",[{}])[0].get("text","")
    if res.get("isError") and any(k in t for k in ("std::","basic_string","out_of_range","json.exception","internal")):
        print("LEAK",n,t[:200],"seed",seed); LEAKS.append(n)
    try: return (res.get("isError"), json.loads(t))
    except: return (res.get("isError"), t)
LEAKS=[]
call("init")
bad=0
for run in range(runs):
    D=os.path.join(work,f"c{run}.docx")
    call("create_document",path=D)
    call("insert_markdown",doc=D,markdown="\n\n".join(f"# H{k}\n\nPara {k} text here.\n\n- a\n- b" for k in range(r.randint(2,6))))
    for step in range(r.randint(10,30)):
        e,rd=call("read_document",doc=D,limit=200)
        ps=[x["id"] for x in rd.get("paragraphs",[])] if isinstance(rd,dict) else []
        if not ps: break
        pid=r.choice(ps)
        tr={"track":r.random()<0.3}
        op=r.randint(0,24)
        a=[
         lambda: call("insert_toc",doc=D,paragraph=pid,**tr),
         lambda: call("add_source",doc=D,type=r.choice(["book","article","website"]),title="T"+str(r.randint(0,9)),authors=["Doe, J"],year="2001"),
         lambda: (lambda s: call("insert_citation",doc=D,paragraph=pid,position=0,source=r.choice(s)["tag"],**tr) if s else None)((call("add_source",doc=D,list=True)[1] or {}).get("sources",[])),
         lambda: call("insert_bibliography",doc=D,paragraph=pid,style=r.choice(["APA","MLA","IEEE","Chicago"]),**tr),
         lambda: call("insert_bibliography",doc=D,style=r.choice(["APA","MLA","IEEE"])),
         lambda: call("define_list",doc=D,preset=r.choice(["numbered","outline","headings","chapters","bullets"]),paragraphs=[pid],**tr),
         lambda: call("insert_chart",doc=D,paragraph=pid,type=r.choice(["column","pie","line","scatter"]),series=[{"values":[1,2,3]}],**tr),
         lambda: call("set_chart_data",doc=D,chart=1,series=[{"values":[r.randint(0,9)]}]),
         lambda: call("insert_shape",doc=D,paragraph=pid,shape=r.choice(["star","line","ellipse","decision"]),text=r.choice(["","x"]),**tr),
         lambda: call("insert_html",doc=D,paragraph=pid,html=r.choice(["<p>x<b>y</b></p>","<ul><li>a<ol><li>b</li></ol></li></ul>","<table><tr><td colspan=2>q</td></tr><tr><td>1</td><td>2</td></tr></table>"]),**tr),
         lambda: call("insert_field",doc=D,paragraph=pid,position=0,code=r.choice(["PAGE","NUMPAGES","SECTIONPAGES","= 1+2","IF 1 = 1 \"y\" \"n\"","MERGEFIELD X","DOCVARIABLE V","SEQ F"]),**tr),
         lambda: call("update_fields",doc=D),
         lambda: call("unlink_fields",doc=D,kinds=r.choice([[],["IF"],["MERGEFIELD"]])),
         lambda: call("theme",doc=D,preset=r.choice(["Green","Violet","Office"])),
         lambda: call("modify_style",doc=D,name="CT",type="table",conditional={"header_row":{"bold":True}}),
         lambda: call("format_table",doc=D,table=1,format=r.choice([{"style":"CT"},{"float":{"x":"right"}},{"float":None}])),
         lambda: call("add_content_control",doc=D,paragraph=pid,type="text",tag="t"+str(r.randint(0,2))),
         lambda: call("bind_content_control",doc=D,control="t"+str(r.randint(0,2)),**r.choice([{"property":"title"},{"data":"a/b"},{}])),
         lambda: call("set_content_control",doc=D,control="t"+str(r.randint(0,2)),value="v"+str(r.randint(0,9))),
         lambda: call("repeating_section",doc=D,first=pid,tag="rs",add=1),
         lambda: call("document_variables",doc=D,set={"V":"x"}),
         lambda: call("set_watermark",doc=D,text="DRAFT"),
         lambda: call("accept_revisions",doc=D),
         lambda: call("reject_revisions",doc=D),
         lambda: call("undo",doc=D),
        ][op]
        a()
    e,s=call("save_document",doc=D)
    if e: bad+=1; print("SAVE",run,str(s)[:300],"seed",seed)
    call("close_document",doc=D)
print("chaos seed",seed,"runs",runs,"save failures",bad,"leaks",len(LEAKS))
sys.exit(1 if bad or LEAKS else 0)
