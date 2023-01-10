# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/)
and this project adheres to [Semantic Versioning](http://semver.org/).

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
