#!/usr/bin/env python3
"""Point every documentation URL in plugin.json at the current version's tag.

Rack stores manualUrl and changelogUrl per plugin and per module, and the
library serves whatever the installed version declares. If those URLs name a
branch, someone running 2.9.0 opens the manual for whatever is on the branch
today and reads about controls their build does not have. So they name a tag,
and this keeps all 26 of them in step with the "version" field.

    python3 tools/release/sync_urls.py           # rewrite to v<version>
    python3 tools/release/sync_urls.py --check   # verify only, nonzero if stale

Release order: bump "version", run this, commit, then tag that commit. The tag
has to be pushed or every link 404s; --check warns when it is missing locally.
Edits the file as text, so key order and \\u escapes survive untouched.
"""

import json
import pathlib
import re
import subprocess
import sys

REPO = "github.com/gosub/forsitan-modulare"
# a documentation URL: .../blob/<ref>/<path>
BLOB = re.compile(r"(" + re.escape(REPO) + r"/blob/)([^/\"]+)(/)")


def main():
    check = "--check" in sys.argv
    root = pathlib.Path(__file__).resolve().parents[2]
    path = root / "plugin.json"
    text = path.read_text(encoding="utf-8")
    data = json.loads(text)
    tag = "v" + data["version"]

    stale = []

    def repl(m):
        if m.group(2) != tag:
            stale.append(m.group(2))
        return m.group(1) + tag + m.group(3)

    updated = BLOB.sub(repl, text)
    total = len(BLOB.findall(text))

    # every module needs a manual, and it has to be one of the URLs above
    missing = [m["slug"] for m in data["modules"] if not m.get("manualUrl")]
    untagged = [m["slug"] for m in data["modules"]
                if "/blob/%s/" % tag not in m.get("manualUrl", "")]
    if not data.get("manualUrl", "").count("/blob/%s/" % tag):
        untagged.append("(plugin manualUrl)")
    if not data.get("changelogUrl", "").count("/blob/%s/" % tag):
        untagged.append("(changelogUrl)")

    if missing:
        print("error: no manualUrl for: %s" % ", ".join(missing))
        return 1

    if check:
        if stale or untagged:
            print("stale: %d of %d URLs do not name %s" % (len(stale), total, tag))
            for ref in sorted(set(stale)):
                print("  found ref %r" % ref)
            for slug in untagged:
                print("  not tagged: %s" % slug)
            print("run tools/release/sync_urls.py to fix")
            return 1
        print("ok: all %d documentation URLs name %s" % (total, tag))
    else:
        if updated != text:
            path.write_text(updated, encoding="utf-8")
            print("rewrote %d URLs to %s (was: %s)"
                  % (len(stale), tag, ", ".join(sorted(set(stale)))))
        else:
            print("ok: all %d documentation URLs already name %s" % (total, tag))

    # the URLs are useless until the tag exists; warn rather than fail, since
    # syncing happens on the commit that is about to be tagged
    try:
        subprocess.check_output(["git", "-C", str(root), "rev-parse", "-q",
                                 "--verify", "refs/tags/" + tag],
                                stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, OSError):
        print("warning: tag %s does not exist yet, so these URLs 404 until "
              "you create and push it" % tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
