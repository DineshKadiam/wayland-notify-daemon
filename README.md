# Fnott

Fnott is a keyboard driven and lightweight notification daemon for
wlroots-based Wayland compositors.

It implements (parts of) the [Desktop Notification Specification](https://developer.gnome.org/notification-spec/).

![screenshot](screenshot.png "Hello World screenshot")

## Supported features

* Summary
* Body
* Actions (requires a dmenu-like utility to display and let user select action)
* Urgency
* Icons (PNG + SVG)
* Markup
* Timeout


## Requirements

### Running

* fontconfig
* freetype
* pixman
* libpng
* wayland (_client_ and _cursor_ libraries)
* wlroots\*
* dbus
* [tllist](https://codeberg.org/dnkl/tllist), _unless_ built as a subproject
* [fcft](https://codeberg.org/dnkl/fcft), _unless_ built as a subproject

\* Fnott must be run in a Wayland compositor that implements the
wlroots protocols.

### Building

In addition to the dev variant of the packages above, you need:

* meson
* ninja
* scdoc
* wayland-protocols


## Usage

Copy the example `fnottrc` to `${HOME}/.config/fnottrc` and edit to
your liking.

Start the daemon by running `fnott`. Note that it does **not**
daemonize or background itself.

Test it with e.g. `notify-send "this is the summary" "this is the body"`.

Use `fnottctl dismiss` to dismiss the highest priority notification
(usually the oldest), `fnottctl dismiss all` to dismiss **all**
notifications, or `fnottctl dismiss <id>` to dismiss a specific
notification (use `fnottctl list` to list currently active
notifications).

You can also click on a notification to dismiss it.

Note: you probably want to bind at least `fnottctl dismiss` to a
keyboard shortcut in your Wayland compositor configuration.
