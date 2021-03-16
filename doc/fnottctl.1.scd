fnottctl(1)

# NAME
fnottctl - utility to interact with *fnott*(1)

# SYNOPSIS
*fnottctl* *dismiss* [_id_]++
*fnottctl* *actions* [_id_]++
*fnottctl* *list*++
*fnottctl* *quit*++
*fnottctl* *--version*

# OPTIONS

*id*
	The notification ID to dismiss or show actions for. Use *fnottctl
	list* to see the IDs of currently active notifications.

*-v*,*--version*
	Show the version number and quit

# DESCRIPTION

*fnottctl* is used to interact (dismiss notifications, show and select
action for notifications) with *fnott*(1).

The most common operation is *fnottctl dismiss*. This will dismiss the
highest priority notification. You might want to bind this to a
keyboard shortcut in your Wayland compositor configuration.

To see, and select between, actions associated with the notification,
use *fnottctl actions*. This requires a dmenu-like utility to have
been configured in *fnott.ini*(5).

You optionally specify a notification ID, to dismiss (or show actions
for) a specific notification instead of the highest priority one.

For _dismiss_, there is also the special ID _all_ which will, not
unsurprisingly, dismiss *all* notifications.

# SEE ALSO

*fnott*(1), *fnott.ini*(5)
