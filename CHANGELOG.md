# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/)
and this project adheres to [Semantic Versioning](http://semver.org/).

## [2.3.0] - 2026-05-09
### Added
  - MMCCCXCIX: PT2399 delay chip emulation with feedback send/return loop
  - MMCCCXCIX: CV inputs for all parameters (time, feedback, mix, brightness, fb loop mix)
  - MMCCCXCIX: external feedback send/return loop with normalled bypass and blend control
  - MMCCCXCIX: soft compressor on wet output to limit self-oscillation amplitude
  - tools: panel-editor.py — browser-based drag-and-drop panel layout editor

## [2.2.1] - 2026-03-14
### Added
  - limen: `list_ports` command to query input/output port names by module id
  - limen: `list_models` command to enumerate available models (optional plugin filter)
  - limen: module filter and verbose mode (`outputModuleName`, `outputPortName`, etc.) for `list_cables`
  - cli: cable id prefix resolution for `disconnect`
  - docs: per-module documentation pages in `doc/`
  - docs: Latin naming section and About page in readme

## [2.2.0] - 2026-03-08
### Added
  - limen: TCP + JSON control interface for controlling VCV Rack from external tools
  - limen: right-click menu to enable/disable server and select TCP port
  - cli: limen command-line client (list modules/plugins, add/remove modules, connect/disconnect cables)

### Fixed
  - limen: Windows (win-x64) build compatibility via Winsock2 shim

## [2.1.0] - 2023-01-10
### Added
  - cumuli: reset input gate and button
  - cumuli: up and down buttons
  - cumuli: monopolar/bipolar selector

## [2.0.2] - 2023-01-01
### Fixed
  - cumuli: corrected z-order of output white outline

### Changed
  - forsitan: updated github action build script (arm64 support)

## [2.0.1] - 2022-02-07
### Added
  - readme: screenshots of v2.0
  - interea: bypass behaviour
  - interea, cumuli, deinde, pavo: labels to lights, inputs, outputs

## [2.0.0] - 2022-01-30
### Changed
 - forsitan: recompiled the plugin with Rack SDK v2.0.x

### Fixed
 - alea: modified module spawn code to compile with Rack v2.0.x

## [1.4.2] - 2021-05-16
### Changed
- alea: simplified panel svg code
- cumuli: simplified panel svg code
- deinde: simplified panel svg code
- pavo: simplified panel svg code

### Fixed
- plugin.json: corrected the link to the CHANGELOG on github
- changelog: fixed sub-headers indentation

## [1.4.1] - 2021-05-13
### Fixed
- interea: the selection of the chord quality when the harmonic option
is on is now based on the V/Octave input only, using thus the frequency
knob as the root note of the scale.
