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

3. **No two modules define the same symbol.**

   ```
   python3 tools/release/check_symbols.py
   ```

   Every module is its own translation unit linked into one plugin, so a
   file-scope type in one `src/*.cpp` shares a namespace with one in another.
   Two of the same name is an ODR violation, and what happens next is the
   linker's choice rather than the code's: MinGW refuses to link and fails the
   Windows job *after* the tag is pushed, while ELF merges them silently and
   the loser's calls quietly run the winner's code.

   That is not hypothetical -- it cost v2.15.0 a re-tag. caligo and materiae
   both declared `TimeQuantity`; caligo defined its member out of line and
   materiae in-class, so on every Linux build materiae's time tooltips ran
   caligo's implementation. Nothing in the test suite touches a tooltip.

   The check reads the object files rather than guessing from the source, so
   it needs a built tree. Put anything file-local in an anonymous namespace.

4. **Panels are legal**, if any layout changed:

   ```
   ~/dl/audio/fonttools-venv/bin/python tools/panel-editor/panel_audit.py
   ```

   No arguments audits every `@layout` module. It checks overlap, label
   offsets, clearances and screw zones with true widget geometry.

5. **Documentation matches the code.** Every module needs `doc/<slug>.md`,
   a row in the readme table, and a `plugin.json` entry. When a module was
   added this release, confirm all of it landed: `src/<name>.cpp`, the model
   declared in `src/forsitan.hpp`, registered in `src/forsitan.cpp`, the
   panel `res/<name>.svg`, the `plugin.json` entry with its `manualUrl`,
   `doc/<slug>.md`, and the readme row.

6. **`plugin.json` metadata.** Every tag must be one Rack knows, and this is
   not a matter of taste: the library rejects a manifest with an unknown tag,
   and it does so *after* the tag is pushed. That has already cost one
   release ([#19](https://github.com/gosub/forsitan-modulare/issues/19),
   `Drone` on four modules and `Tape` on a fifth). Check before tagging:

   ```
   python3 tools/release/check_tags.py
   ```

   Nonzero on an unknown tag, and it warns about aliases -- `Synth Voice`
   where Rack says `Synth voice` -- which the library accepts but which read
   as typos. It works offline against a cached copy of Rack's own list;
   `--fetch` refreshes that copy from `src/tag.cpp` upstream and reports what
   changed. Worth running after a Rack release.

   Beyond validity, tags should still *describe* the module, since library
   browse-by-tag is a discovery channel. Descriptions are **one-line
   summaries**: Rack renders the field as the module-browser hover tooltip and
   does not wrap it, so keep them under ~100 characters and leave behaviour to
   the manual.

7. **Regenerate anything derived**, if its source changed:

   ```
   python3 tools/patches/gen_patches.py      # patches/*.vcv
   ```

8. **Update the images** if any panel changed. Both kinds are generated,
   never screenshotted by hand:

   ```
   python3 tools/release/gen_screenshots.py     # img/<module>.png, or name modules
   python3 tools/release/gen_collection.py      # img/forsitan-modulare.png
   ```

   `gen_collection.py` drives the running Rack through limen, so the plugin
   installed for it must speak limen protocol 2.

9. **Write the CHANGELOG entry.** Heading `## [<version>] - <YYYY-MM-DD>`,
   at the top, grouped `### Added` / `### Changed` / `### Fixed`. `git log`
   since the previous tag is the raw material.

10. **Bump `"version"` in `plugin.json`,** then make everything agree:

   ```
   python3 tools/release/sync_version.py
   ```

   This repoints all 26 documentation URLs (plugin `manualUrl`, one per
   module, and `changelogUrl`) at `v<version>`. They name a tag rather than
   a branch so that someone running an older build opens the manual their
   build actually matches. Never hand-edit them.

11. **Verify, then commit.**

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
HOME=/home/gg/dl/audio/rackhome/ make install
cd ~/dl/audio/rack && HOME=/home/gg/dl/audio/rackhome/ ./Rack
```

The fake `HOME` keeps the real one clean; run Rack with the same one so it
loads what you just installed.
