"""Validate the standalone review artifact; this does not test the native DLL."""
from pathlib import Path
from html.parser import HTMLParser
import base64
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HTML = ROOT / "docs/design/PROTOTYPE.html"


class ArtifactParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = []
        self.urls = []
        self.images = []
        self.buttons = []

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if "id" in attrs:
            self.ids.append(attrs["id"])
        for key in ("src", "href"):
            if key in attrs:
                self.urls.append(attrs[key])
        if tag == "img":
            self.images.append(attrs)
        if tag == "button":
            self.buttons.append(attrs)


def main():
    content = HTML.read_text(encoding="utf-8")
    parser = ArtifactParser()
    parser.feed(content)
    assert len(parser.ids) == len(set(parser.ids)), "Duplicate DOM ids"
    script = re.search(r"<script>(.*?)</script>", content, re.S).group(1)
    refs = set(re.findall(r"\$\('([^']+)'\)", script))
    assert refs <= set(parser.ids), f"Missing DOM ids: {refs - set(parser.ids)}"
    assert all(not re.match(r"(?:https?:)?//", url) for url in parser.urls), "External asset"
    assert not re.search(r"@import|url\(\s*['\"]?https?://", content), "Remote CSS"
    assert all(img.get("alt") for img in parser.images), "Missing image alt"
    logo = next(img for img in parser.images if img.get("id") == "appLogo")
    assert base64.b64decode(logo["src"].split(",", 1)[1], validate=True) == (ROOT / "input/LOGO.png").read_bytes(), "Logo changed"
    assert "__LOGO_BASE64__" not in content, "Unresolved image placeholder"
    assert 'aria-live="polite"' in content, "Missing accessible feedback"
    assert "prefers-reduced-motion" in content, "Missing reduced motion handling"
    node = shutil.which("node")
    assert node, "Node.js is required for JavaScript syntax validation"
    with tempfile.TemporaryDirectory(prefix="gwm-prototype-check-") as temp:
        js = Path(temp) / "prototype.js"
        js.write_text(script, encoding="utf-8")
        subprocess.run([node, "--check", str(js)], check=True)
    for name in ("REQUIREMENTS.md", "SCOPE.md", "ARCHITECTURE.md", "API_SPEC.md", "OPEN_SOURCE_RESEARCH.md", "IMPLEMENTATION_PLAN.md", "TEST_PLAN.md", "TEST_REPORT.md", "design/UI_SPEC.md"):
        text = (ROOT / "docs" / name).read_text(encoding="utf-8")
        assert "待补充" not in text, f"Unfinished template: {name}"
    print(f"PASS: {len(parser.ids)} unique DOM ids; all literal references resolved")
    print("PASS: standalone resources; original logo bytes preserved")
    print("PASS: JavaScript syntax; accessibility hooks; project documents populated")
    print("LIMIT: browser interactions, native capture and DLL ABI require separate testing")


if __name__ == "__main__":
    main()
