# Changelog

* [Unreleased](#unreleased)
* [1.0.1](#1-0-1)
* [1.0.0](#1-0-0)


## Unreleased
### Added

* Configurable minimal width of notifications. New `fnottrc` option:
  `min-width`
* Configurable anchor point and margins. New `fnottrc` options:
  `anchor=top-left|top-right|bottom-left|bottom-right`,
  `edge-margin-vertical`, `edge-margin-horizontal` and
  `notification-margin` (https://codeberg.org/dnkl/fnott/issues/4).
* `-c,--config=PATH` command line option
  (https://codeberg.org/dnkl/fnott/issues/10).
* Text shaping support (https://codeberg.org/dnkl/fnott/issues/13).


### Changed

* Fnott now searches for its configuration in
  `$XDG_DATA_DIRS/fnott/fnott.ini`, if no configuration is found in
  `$XDG_CONFIG_HOME/fnott/fnott.ini` or in `$XDG_CONFIG_HOME/fnottrc`
  (https://codeberg.org/dnkl/fnott/issues/7).
* Assume a DPI of 96 if the monitor’s DPI is 0 (seen on certain
  emulated displays).
* There is now an empty line between the ‘summary’ and ‘body’.


### Deprecated

* `$XDG_CONFIG_HOME/fnottrc` and `~/.config/fnottrc`. Use
  `$XDG_CONFIG_HOME/fnott/fnott.ini` (defaulting to
  `~/.config/fnott/fnott.ini`) instead
  (https://codeberg.org/dnkl/fnott/issues/7).


### Removed

* `margin` option from `fnottrc`


### Fixed

* Notification sometimes not being rendered with the correct subpixel
  mode, until updated.


### Security
### Contributors

- yyp (Alexey Yerin)

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
