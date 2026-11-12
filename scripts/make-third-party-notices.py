"""Makes licenses/THIRD_PARTY_NOTICES.txt from the credits that Mozc ships.

Mozc's installer shows data/installer/credits_en.html: the licenses of Mozc,
its dictionary data and its libraries. This script turns that file into plain
text. The Qt entry is left out, because the executable of this repository does
not use Qt.

usage: python scripts/make-third-party-notices.py
"""
import html
import pathlib
import re
import sys

EXCLUDE = {"Qt Base"}
# Libraries that the executable may contain but Mozc's credits do not list
# (found with `bazel query "deps(//mozc_inproc:mozc_skkserv)"`): title,
# homepage and the file with the license text (relative to licenses/extra).
EXTRA = [("zlib", "https://zlib.net/", "zlib.txt")]

repo = pathlib.Path(__file__).resolve().parent.parent
source = repo / "mozc" / "src" / "data" / "installer" / "credits_en.html"
target = repo / "licenses" / "THIRD_PARTY_NOTICES.txt"

text = source.read_text(encoding="utf-8")
products = re.findall(
    r'<div class="product">\s*<span class="title">(.*?)</span>\s*'
    r'<span class="homepage">\s*<a href="(.*?)">.*?</span>\s*'
    r'<div class="licence">\s*<h3>.*?</h3>\s*<pre>(.*?)</pre>',
    text,
    re.S,
)
if not products:
    sys.exit("no entry found in " + str(source))

out = [
    "THIRD-PARTY NOTICES",
    "",
    "The executable of mozc-skkserv contains Mozc, the dictionary data of Mozc",
    "and other libraries. Their licenses follow. The texts are those that Mozc",
    "ships in src/data/installer/credits_en.html.",
    "",
]
names = []
for title, homepage, body in products:
    title = html.unescape(title).strip()
    if title in EXCLUDE:
        continue
    names.append(title)
    out += [
        "=" * 78,
        title,
        html.unescape(homepage).strip(),
        "=" * 78,
        "",
        html.unescape(body).strip("\r\n"),
        "",
    ]

for title, homepage, name in EXTRA:
    names.append(title)
    body = (repo / "licenses" / "extra" / name).read_text(encoding="utf-8")
    out += ["=" * 78, title, homepage, "=" * 78, "", body.strip("\r\n"), ""]

target.parent.mkdir(exist_ok=True)
target.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
print("wrote", target)
for n in names:
    print(" -", n)
