# Releasing forsitan modulare

The authoritative release procedure. CI does most of the work: pushing a
`v*` tag builds all four platforms, creates the GitHub release and attaches
the artifacts. What is left here is getting the repository into a consistent
state *before* the tag, and telling the VCV Library about it after.

## Version numbers

| bump | when |
|------|------|
| **minor** `2.x.0` | a new module is added |
| **patch** `2.x.y` | fixes and enhancements to existing modules |

`plugin.json` `"version"` and the git tag must match exactly, tag prefixed
with `v`: version `2.12.1` is tag `v2.12.1`. CI's publish job compares them
and fails the release if they differ, so a mismatch costs a re-tag rather
than a bad release.

## Before the tag

1. **Green build.** Every push builds win-x64, lin-x64, mac-x64 and
   mac-arm64. Check the run for the commit you intend to tag.

2. **Tests pass.** `make -C test -j && make -C test check`, which runs one
   smoke binary per module and exits nonzero on any failure. Note the runs
   are not reproducible: the harnesses seed from the clock.

3. **Panels are legal**, if any layout changed:

   ```
   ~/dl/audio/fonttools-venv/bin/python tools/panel-editor/panel_audit.py
   ```

   No arguments audits every `@layout` module. It checks overlap, label
   offsets, clearances and screw zones with true widget geometry.

4. **Documentation matches the code.** Every module needs `doc/<slug>.md`,
   a row in the readme table, and a `plugin.json` entry. When a module was
   added this release, confirm all of it landed: `src/<name>.cpp`, the model
   declared in `src/forsitan.hpp`, registered in `src/forsitan.cpp`, the
   panel `res/<name>.svg`, the `plugin.json` entry with its `manualUrl`,
   `doc/<slug>.md`, and the readme row.

5. **`plugin.json` metadata.** Tags still describe the module (`tags` drives
   library filtering). Descriptions are **one-line summaries**: Rack renders
   the field as the module-browser hover tooltip and does not wrap it, so
   keep them under ~100 characters and leave behaviour to the manual.

6. **Regenerate anything derived**, if its source changed:

   ```
   python3 tools/patches/gen_patches.py      # patches/*.vcv
   ```

7. **Update the images** if any panel changed. The per-module images in
   `img/` come from Rack's own renderer, never from a manual screenshot:

   ```
   python3 tools/release/gen_screenshots.py          # or name the modules
   ```

   The readme screenshot (`img/forsitan-modulare.png`) is still taken by
   hand when the lineup changes.

8. **Write the CHANGELOG entry.** Heading `## [<version>] - <YYYY-MM-DD>`,
   at the top, grouped `### Added` / `### Changed` / `### Fixed`. `git log`
   since the previous tag is the raw material.

9. **Bump `"version"` in `plugin.json`,** then make everything agree:

   ```
   python3 tools/release/sync_version.py
   ```

   This repoints all 26 documentation URLs (plugin `manualUrl`, one per
   module, and `changelogUrl`) at `v<version>`. They name a tag rather than
   a branch so that someone running an older build opens the manual their
   build actually matches. Never hand-edit them.

10. **Verify, then commit.**

    ```
    python3 tools/release/sync_version.py --check
    ```

    Fails if `plugin.json`, the newest CHANGELOG heading and the URLs
    disagree. It warns that the tag does not exist yet; that is expected
    here, and it never fails the check. Commit the result.

## Tag and publish

```
git tag v<version>
git push && git push --tags
```

The tag must be pushed. Until it is, all 26 documentation URLs 404, since
they name it. Nothing else depends on the tag, so local development never
needs one.

CI then verifies version against tag, builds the four platforms, creates the
GitHub release and uploads every `.vcvplugin`. Check the release page.

## Tell the VCV Library

The plugin lives at **[VCVRack/library issue #681](https://github.com/VCVRack/library/issues/681)**.
Updates are a comment on that issue, in this form:

```
update:

https://github.com/gosub/forsitan-modulare
version: <version>
commit: <the tagged commit sha>
```

Get the sha with `git rev-list -n 1 v<version>`. The library builds from
source on its own infrastructure, so this is what actually puts the release
in front of users; a tag alone does not.

## Local install, for checking a build by hand

```
HOME=/home/gg/dl/temp/rackhome/ make install
cd ~/dl/audio/rack && HOME=/home/gg/dl/temp/rackhome/ ./Rack
```

The fake `HOME` keeps the real one clean; run Rack with the same one so it
loads what you just installed.
