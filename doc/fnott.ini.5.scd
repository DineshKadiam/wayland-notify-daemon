fnott(5)

# NAME
fnott - configuration file

# DESCRIPTION

*fnott* uses the standard _unix configuration format_, with section
based key/value pairs. The default (global) section is unnamed
(i.e. not prefixed with a _[section]_).

fnott will search for a configuration file in the following locations,
in this order:

- *XDG_CONFIG_HOME/fnott/fnott.ini*
- *~/.config/fnott/fnott.ini*
- *XDG_CONFIG_DIRS/fnott/fnott.ini*

# SECTION: default

## Global options

*output*
	The output to display notifications on. If left unspecified, the
	compositor will choose one for us.
	
	Note that if you *do not* specify an output, and the output chosen
	by the compositor is scaled, then each new notification will flash
	a low-res frame before re-rendering with the correct scale
	factor. This is because fnott has no way of knowing what the scale
	factor is until *after* the notification has been mapped
	(i.e. shown).
	
	Default: _unspecified_.
	
	In Sway, you can use *swaymsg -t get_outputs* to get a list of
	available outputs.

*min-width*
	Minimum notification width, in pixels. Default: _0_.

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

*stacking-order*
	How to stack multiple notifications.
	
		- *bottom-up* : oldest, highest priority furthest away from
		  the anchor point.
		- *top-down*: oldest, highest priority at the anchor point.
	
	Thus, if the notifications are anchored at the top, *bottom-up*
	will have the most recent notification in the upper corner, while
	the oldest notification is in the bottom of the stack.
	
	Default: _bottom-up_.

*anchor*
	Where notifications are positioned: *top-left*, *top-right*,
	*bottom-left* or *bottom-right*. Default: _top-right_.

*edge-margin-vertical*
	Vertical margin, in pixels, between the screen edge (top or
	bottom, depending on anchor pointer) and notifications. Default:
	_10_.

*edge-margin-horizontal*
	Horizontal margin, in pixels, between the screen edge (left or
	right, depending on anchor pointer) and notifications. Default:
	_10_.

*notification-margin*
	Margin between notifications. Default: _10_.

*selection-helper*
	Command (and optionally arguments) to execute to display actions
	and let the user select an action to run.
	
	The utility should accept (action) entries to display on stdin
	(newline separated), and write the selected entry on
	stdout. Default: _dmenu_.

*play-sound*
	Command to execute to play notification sounds. _${filename}_ will
	be expanded to the path to the audio file to play. Default: _aplay
	${filename}_.

## Per-urgency default options

These options can also be specified in an _urgency_ section, in which
case they override the values specified in the default section.

*background*
	Background color of the notification, in RGBA format. Default:
	_3f5f3fff_.

*border-color*
	Border color of the notification, in RGBA format. Default:
	_909090ff_.

*border-size*
	Border size, in pixels. Default: _1_.

*padding-vertical*
	Vertical padding, in pixels, between the notification edge (top or
	bottom) and notification text. Default: _20_.

*padding-horizontal*
	Horizontal padding, in pixels, between the notification edge (left
	or right) and notification text. Default: _20_.

*title-font*
	Font to use for the application title, in fontconfig format (see
	*FONT FORMAT*). Default: _sans serif_.

*title-color*
	Text color to use for the application title, in RGBA
	format. Default: _ffffffff_.

*title-format*
	Template string for the title portion of the notification (see
	*FORMAT STRINGS*). Default: _<i>%a%A</i>_.

*summary-font*
	Font to use for the summary, in fontconfig format (see *FONT
	FORMAT*). Default: _sans serif_.

*summary-color*
	Text color to use for the summary, in RGBA format. Default:
	_ffffffff_.

*summary-format*
	Template string for the summary portion of the notification (see
	*FORMAT STRINGS*). Default: _<b>%s\\n</b>_.

*body-font*
	Font to use for the text body, in fontconfig format (see *FONT
	FORMAT*). Default: _sans serif_.

*body-color*
	Text color to use for the text body, in RGBA format. Default:
	_ffffffff_.

*body-format*
	Template string for the body portion of the notification (see
	*FORMAT STRINGS*). Default: _%b_.

*progress-bar-height*
	Height, in pixels, of progress bars (rendered when a notification
	has an _int:value_ hint). Default: _20_.

*progress-bar-color*
	Color, in RGBA format, of progress bars. Default: _ffffffff_.

*max-timeout*
	Time limit, in seconds, before notifications are automatically
	dismissed. Applications can provide their own timeout when they
	create a notification. This option can be used to limit that
	timeout. A value of 0 disables the limit. Default: _0_.

*default-timeout*
	Time, in seconds, before notifications are automatically dismissed
	if the notifying application does not specify a timeout. A value
	of 0 disables the timeout. I.e. if the application does not
	provide a timeout, the notification is never automatically
	dismissed (unless *max-timeout* has been set). Default: _0_.

*sound-file*
	Absolute path to audio file to play when a notification is
	received. If unset, no sound is played. Default: _unset_.

# SECTION: low

This section allows you to override the options listed under
*per-urgency default options* for _low_ priority notifications.

By default, the following options are already overridden:

- *background*: _2b2b2bff_
- *title-color* _888888ff_
- *summary-color*: _888888ff_
- *body-color*: _888888ff_

# SECTION: normal

This section allows you to override the options listed under
*per-urgency default options* for _normal_ priority notifications.

By default, the following options are already overridden: _none_.

# SECTION: critical

This section allows you to override the options listed under
*per-urgency default options* for _critical_ priority notifications.

By default, the following options are already overridden:

- *background*: _6c3333ff_


# FONT FORMAT

The font is specified in FontConfig syntax. That is, a colon-separated
list of font name and font options.

_Examples_:
- Dina:weight=bold:slant=italic
- Courier New:size=12

# FORMAT STRINGS

The *title-format*, *summary-format* and *body-format* options allow
you to configure what to display for the corresponding portion of the
notification.

They are strings with placeholders that are expanded with attributes
from the notification:

- *%a*  application name
- *%s*  notification summary
- *%b*  notification body text
- *%A*  action indicator ('\*' if actions are present, empty string otherwise)
- *%%*  a literal '%'
- *\\n*  a literal newline

Also supported are the following markup tags:

- *<b>*  bold
- *<i>*  italic
- *<u>*  underline


# SEE ALSO

*fnott*(1)
