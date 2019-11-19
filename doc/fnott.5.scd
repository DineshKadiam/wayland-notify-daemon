fnott(5)

# NAME
fnott - configuration file

# DESCRIPTION

*fnott* uses the standard _unix configuration format_, with section
based key/value pairs. THe default (global) section is unnamed
(i.e. not prefixed with a _[section]_).

fnott will search for a configuration file in the following locations,
in this order:

- _XDG_CONFIG_HOME/fnottrc_
- _~/.config/fnottrc_

# SECTION: default

## Global options

*output*
	The output to display notifications on. If left unspecified, the
	"last" detected output will be used. Default: _unspecified_.
	
	In Sway, you can use *swaymsg -t get_outputs* to get a list of
	available outputs.

*max-width*
	Maximum notification width, in pixels. Default: _400_.

*max-height*
	Maximum notification height, in pixels. Default: _200_.

*icon-theme*
	Icon theme to use when a notification has requested an
	non-embedded icon. Default: _hicolor_.


*max-icon-size*
	Maximum icon size, in pixels. Icons larger than this will be
	scaled down. Default: _32_.

*margin*
	Space, in pixels, added around each notification (but note that it
	is only applied _once_ between two notifications). Default: _10_.

*selection-helper*
	Command (and optionally arguments) to execute to display actions
	and let the user select an action to run.
	
	The utility should accept (action) entries to display on stdin
	(newline separated), and write the selected entry on
	stdout. Default: _dmenu_.

## Per-urgency default options

These options can also be specified in an _urgency_ section, in which
case they override the values specified in the default section.

*background*
	Background color of the notification. Default: _3f5f3f_.

*border-color*
	Border color of the notification. Default: _909090_.

*border-size*
	Border size, in pixels. Default: _1_.

*title-font*
	Font to use for the application title, in fontconfig format (see
	*FONT FORMAT*). Default: _sans serif_.

*title-color*
	Text color to use for the application title. Default: _ffffff_.

*summary-font*
	Font to use for the summary, in fontconfig format (see *FONT
	FORMAT*). Default: _sans serif_.

*summary-color*
	Text color to use for the summary. Default: _ffffff_.

*body-font*
	Font to use for the text body, in fontconfig format (see *FONT
	FORMAT*). Default: _sans serif_.

*body-color*
	Text color to use for the text body. Default: _ffffff_.

# SECTION: low

This section allows you to override the options listed under
*per-urgency default options* for _low_ priority notifications.

By default, the following options are already overriden:

- *background*: _2b2b2b_
- *title-color* _888888_
- *summary-color*: _888888_
- *body-color*: _888888_

# SECTION: normal

This section allows you to override the options listed under
*per-urgency default options* for _normal_ priority notifications.

By default, the following options are already overriden: _none_.

# SECTION: critical

This section allows you to override the options listed under
*per-urgency default options* for _critical_ priority notifications.

By default, the following options are already overriden:

- *background*: _6c3333_


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12

# SEE ALSO

*fnott*(1)
