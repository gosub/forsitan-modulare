#!/usr/bin/env python3
"""Keep the release version consistent across plugin.json and CHANGELOG.md.

Three things have to agree on tag day: the "version" in plugin.json, the top
heading in CHANGELOG.md, and the 26 documentation URLs Rack serves from the
manifest (plugin manualUrl, one per module, and changelogUrl). Those URLs name
the version's tag rather than a branch, because the library hands out whatever
the installed build declares: a branch URL shows someone running 2.9.0 the
manual for whatever is on master today.

    python3 tools/release/sync_version.py           # rewrite URLs to v<version>
    python3 tools/release/sync_version.py --check   # verify only, nonzero if stale

--check is the tag-day gate: it fails on the classic mistake of bumping the
version but leaving the changelog heading or a URL behind. CI enforces the
version/tag match separately, in the publish job.

Release order: bump "version", write the CHANGELOG entry, run this, commit,
then tag that commit and push the tag. The tag only has to exist when the
version is actually published, so the missing-tag notice is a warning and
never fails --check: local development does not need the links to resolve.

Edits plugin.json as text, so key order and the \\u escape in dræn survive.
"""

import json
import pathlib
import re
import subprocess
import sys

REPO = "github.com/gosub/forsitan-modulare"
# a documentation URL: .../blob/<ref>/<path>
BLOB = re.compile(r"(" + re.escape(REPO) + r"/blob/)([^/\"]+)(/)")
# "## [2.12.1] - 2026-07-25"
HEADING = re.compile(r"^## \[([^\]]+)\]", re.M)


def check_changelog(root, version):
    """The newest CHANGELOG heading must be this version. Returns problems."""
    path = root / "CHANGELOG.md"
    if not path.exists():
        return ["CHANGELOG.md is missing"]
    headings = HEADING.findall(path.read_text(encoding="utf-8"))
    if not headings:
        return ["CHANGELOG.md has no '## [version]' heading"]
    if headings[0] != version:
        return ["CHANGELOG.md newest entry is [%s], plugin.json says %s"
                % (headings[0], version)]
    if headings.count(version) > 1:
        return ["CHANGELOG.md has %d entries for %s"
                % (headings.count(version), version)]
    return []


def tag_exists(root, tag):
    try:
        subprocess.check_output(["git", "-C", str(root), "rev-parse", "-q",
                                 "--verify", "refs/tags/" + tag],
                                stderr=subprocess.DEVNULL)
        return True
    except (subprocess.CalledProcessError, OSError):
        return False


def main():
    check = "--check" in sys.argv
    root = pathlib.Path(__file__).resolve().parents[2]
    path = root / "plugin.json"
    text = path.read_text(encoding="utf-8")
    data = json.loads(text)
    version = data["version"]
    tag = "v" + version

    stale = []

    def repl(m):
        if m.group(2) != tag:
            stale.append(m.group(2))
        return m.group(1) + tag + m.group(3)

    updated = BLOB.sub(repl, text)
    total = len(BLOB.findall(text))

    problems = []
    missing = [m["slug"] for m in data["modules"] if not m.get("manualUrl")]
    if missing:
        problems.append("no manualUrl for: %s" % ", ".join(missing))
    if not data.get("manualUrl"):
        problems.append("no plugin-level manualUrl")
    if not data.get("changelogUrl"):
        problems.append("no changelogUrl")

    if check:
        problems += check_changelog(root, version)
        if stale:
            problems.append("%d of %d documentation URLs do not name %s (found: %s)"
                            % (len(stale), total, tag, ", ".join(sorted(set(stale)))))
        if problems:
            print("version %s: NOT consistent" % version)
            for p in problems:
                print("  - %s" % p)
            print("run tools/release/sync_version.py to repoint the URLs")
            return 1
        print("version %s: plugin.json, CHANGELOG.md and all %d URLs agree"
              % (version, total))
    else:
        if problems:
            print("error:")
            for p in problems:
                print("  - %s" % p)
            return 1
        if updated != text:
            path.write_text(updated, encoding="utf-8")
            print("rewrote %d URLs to %s (was: %s)"
                  % (len(stale), tag, ", ".join(sorted(set(stale)))))
        else:
            print("all %d documentation URLs already name %s" % (total, tag))

    if not tag_exists(root, tag):
        print("note: tag %s does not exist yet. The URLs resolve once you tag "
              "and push; nothing to do for local development." % tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
