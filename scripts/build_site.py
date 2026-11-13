#!/usr/bin/env python3
"""Builds the GitHub Pages site for the UML diagrams.

    scripts/build_site.py --svg-dir build/uml --out site [--sha SHA] [--date D]

Reads the rendered *.svg files from --svg-dir and writes the site to --out:

    index.html   the listing page, generated here
    style.css    copied from docs/site/
    uml/*.svg    the diagrams

`gen_*` diagrams are generated from the code (clang-uml); everything else is
hand-authored. descriptions.json may set "group" (overview, narrative, detail). Page text and styling live in docs/site/ (style.css,
descriptions.json) so they can be changed without touching this script.
Standard library only.
"""
import argparse
import datetime
import html
import json
import re
import shutil
import sys
from pathlib import Path

REPO_URL = "https://github.com/fujinet-nio/fujinet-nio"
SITE_SRC = Path(__file__).resolve().parent.parent / "docs" / "site"


def title_for(stem: str) -> str:
    """CoreIOArchitecture -> 'Core IO Architecture'; seq_atari_x -> 'Atari x'."""
    name = re.sub(r"^(gen|arch|seq)_", "", stem)
    if "_" in name:
        name = name.replace("_", " ")
    else:
        name = re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", " ", name)
    return name[:1].upper() + name[1:]


def kind_for(stem: str) -> str:
    low = stem.lower()
    if "seq" in low or "sequence" in low or "flow" in low:
        return "sequence"
    return "structure"


def card(svg: Path, descriptions: dict) -> str:
    stem = svg.stem
    info = descriptions.get(stem, {})
    title = html.escape(info.get("title") or title_for(stem))
    desc = html.escape(info.get("description", ""))
    href = f"uml/{html.escape(svg.name)}"
    kind = kind_for(stem)
    return f"""      <a class="card" href="{href}">
        <span class="thumb"><img src="{href}" alt="" loading="lazy"></span>
        <span class="body">
          <span class="title">{title}</span>
          <span class="badge {kind}">{kind}</span>
          <span class="desc">{desc}</span>
          <span class="file">{html.escape(svg.name)}</span>
        </span>
      </a>"""


def section(heading: str, blurb: str, svgs: list, descriptions: dict) -> str:
    if not svgs:
        return ""
    cards = "\n".join(card(s, descriptions) for s in svgs)
    return f"""    <section>
      <h2>{html.escape(heading)}</h2>
      <p class="blurb">{html.escape(blurb)}</p>
      <div class="grid">
{cards}
      </div>
    </section>
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--svg-dir", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--sha", default="")
    ap.add_argument("--date", default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"))
    args = ap.parse_args()

    svgs = sorted(args.svg_dir.glob("*.svg"))
    if not svgs:
        print(f"no .svg files in {args.svg_dir}", file=sys.stderr)
        return 1

    descriptions = {}
    desc_file = SITE_SRC / "descriptions.json"
    if desc_file.exists():
        descriptions = json.loads(desc_file.read_text(encoding="utf-8"))

    def group_of(svg: Path) -> str:
        info = descriptions.get(svg.stem, {})
        if "group" in info:
            return info["group"]
        return "detail" if svg.stem.startswith("gen_") else "narrative"

    # Within a group, follow the order of descriptions.json, then by name.
    position = {name: i for i, name in enumerate(descriptions)}
    svgs.sort(key=lambda f: (position.get(f.stem, len(position)), f.name))

    groups = {"overview": [], "narrative": [], "detail": []}
    for s in svgs:
        groups.setdefault(group_of(s), []).append(s)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "uml").mkdir(exist_ok=True)
    for s in svgs:
        shutil.copyfile(s, args.out / "uml" / s.name)
    shutil.copyfile(SITE_SRC / "style.css", args.out / "style.css")

    sha = args.sha.strip()
    built = f"Built {html.escape(args.date)}"
    if sha:
        built += f' from <a href="{REPO_URL}/commit/{html.escape(sha)}"><code>{html.escape(sha[:8])}</code></a>'

    page = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>fujinet-nio diagrams</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <header>
    <div class="wrap">
      <h1>fujinet-nio <span>diagrams</span></h1>
      <p class="sub">Architecture and code structure of the FujiNet network adapter firmware.
        <a href="{REPO_URL}">Repository</a> &middot;
        <a href="{REPO_URL}/blob/master/docs/architecture.md">Architecture document</a></p>
    </div>
  </header>
  <main class="wrap">
{section("Overviews", "Generated from the code on every push, so they follow it. Members are hidden; these show the types and how they relate.", groups["overview"], descriptions)}{section("Narrative", "Hand-authored: flows and layouts that cannot be generated. Their names are checked against the code.", groups["narrative"], descriptions)}{section("Detail", "Generated class and sequence diagrams with members. Large; use for reference.", groups["detail"], descriptions)}
  </main>
  <footer>
    <div class="wrap">{built}</div>
  </footer>
</body>
</html>
"""
    (args.out / "index.html").write_text(page, encoding="utf-8")
    print(f"wrote {args.out}/index.html ({len(groups['overview'])} overviews, {len(groups['narrative'])} narrative, {len(groups['detail'])} detail)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
