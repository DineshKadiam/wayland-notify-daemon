# Changelog

* [Unreleased](#unreleased)
* [1.0.1](#1-0-1)
* [1.0.0](#1-0-0)


## Unreleased
### Added
### Changed
### Deprecated
### Removed
### Fixed
### Security
### Contributors


## 1.0.1

### Added

* `timeout` option to `fnottrc`. This option can be set on a
  per-urgency basis. If both the user has set a timeout, and the
  notification provides its own timeout, the shortest one is used
  (https://codeberg.org/dnkl/fnott/issues/2).
* FreeBSD port (https://codeberg.org/dnkl/fnott/issues/1).


### Fixed

* PPI being incorrectly calculated.
* Crash due to bug in Sway-1.5 when a notification is dismissed,
  either with `fnottctl` or through its timeout, while the cursor is
  above it.


### Contributors

* jbeich


## 1.0.0

Initial release - no changelog. Rough list of features:

* Application title, summary and body fonts can be configured individually
* Icon support, both inline and name referenced (PNG + SVG).
* Actions (requires a dmenu-like utility to display and let user
  select action - e.g. [fuzzel](https://codeberg.org/dnkl/fuzzel))
* Urgency (custom colors and fonts for different urgency levels)
* Markup (**bold**, _italic_ and underline)
* Timeout (notification is automatically dismissed)
