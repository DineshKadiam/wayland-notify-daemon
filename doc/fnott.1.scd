fnott(1)

# NAME
fnott - keyboard driven notification daemon for Wayland

# SYNOPSIS
*fnott*++
*fnott* *--version*

# OPTIONS

*-p*,*--print-pid*=_FILE_|_FD_
	Print PID to this file, or FD, when successfully started. The file
	(or FD) is closed immediately after writing the PID. When a _FILE_
	as been specified, the file is unlinked when yambar exits.

*-l*,*--log-colorize*=[{*never*,*always*,*auto*}]
	Enables or disables colorization of log output on stderr.

*-s*,*--log-no-syslog*
	Disables syslog logging. Logging is only done on stderr.

*-v*,*--version*
	Show the version number and quit

# DESCRIPTION

*fnott* is a keyboard driven and lightweight notification daemon for
wlroots-based Wayland compositors, implementing (parts of) the Desktop
Notification Specification.

*fnott* is the daemon part and should be launced when you log in to
your desktop. You should be able to configure your Wayland compositor
to autolaunch it for you.

With the daemon running, you will be able to receive and display
notifications:

	notify-send "this is the summary" "this is the body"

Use *fnottctl*(1) to interact with it:

	fnottctl dismiss

Or simply click on a notification to dismiss it.

Notifications are prioritized in the following way:

- urgency - normal notifications always have higher priority than
  low-urgency notifications, and critical notifications always have
  higher priority than normal notifications.
- time - older notifications have higher priority than newer
  notifications.

Notifications are displayed with the lowest priority ones at the top,
and the highest priority ones at the bottom (this is subject to
change).

# CONFIGURATION

See *fnott*(5)

# SEE ALSO

*fnott*(5), *fnottctl*(1)
