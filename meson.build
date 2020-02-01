project('fnott', 'c',
        version: '0.0.0',
        license: 'MIT',
        meson_version: '>=0.47.0',
        default_options: [
          'c_std=c11',
          'warning_level=1',
          'werror=true',
          'b_ndebug=if-release'])

is_debug_build = get_option('buildtype').startswith('debug')

add_project_arguments(
  ['-D_GNU_SOURCE=200809L'] + (is_debug_build ? ['-D_DEBUG'] : []),
  language: 'c')

cc = meson.get_compiler('c')
math = cc.find_library('m', required : false)

threads = dependency('threads')
pixman = dependency('pixman-1')
png = dependency('libpng')
wayland_protocols = dependency('wayland-protocols')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
dbus = dependency('dbus-1')

tllist = dependency('tllist', version: '>=1.0.0', fallback: ['tllist', 'tllist'])
fcft = dependency('fcft', version: ['>=1.1.0', '<1.2.0'], fallback: ['fcft', 'fcft'])

wayland_protocols_datadir = wayland_protocols.get_pkgconfig_variable('pkgdatadir')

wscanner = dependency('wayland-scanner', native: true)
wscanner_prog = find_program(
  wscanner.get_pkgconfig_variable('wayland_scanner'), native: true)

wl_proto_headers = []
wl_proto_src = []
foreach prot : [
  'external/wlr-layer-shell-unstable-v1.xml',
  wayland_protocols_datadir + '/stable/xdg-shell/xdg-shell.xml',
  wayland_protocols_datadir + '/unstable/xdg-output/xdg-output-unstable-v1.xml',
  ]

  wl_proto_headers += custom_target(
    prot.underscorify() + '-client-header',
    output: '@BASENAME@.h',
    input: prot,
    command: [wscanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'])

  wl_proto_src += custom_target(
    prot.underscorify() + '-private-code',
    output: '@BASENAME@.c',
    input: prot,
    command: [wscanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

generate_version_sh = files('generate-version.sh')
version = custom_target(
  'generate_version',
  build_always_stale: true,
  output: 'version.h',
  command: [generate_version_sh, meson.project_version(), '@SOURCE_DIR@', '@OUTPUT@'])

nanosvg = declare_dependency(
  sources: ['nanosvg.c', '3rd-party/nanosvg.h', 'nanosvgrast.c', '3rd-party/nanosvgrast.h'],
  dependencies: math)

executable(
  'fnott',
  'main.c',
  'config.c', 'config.h',
  'ctrl.c', 'ctrl.h',
  'dbus.c', 'dbus.h',
  'fdm.c', 'fdm.h',
  'icon.c', 'icon.h',
  'log.c', 'log.h',
  'notification.c', 'notification.h',
  'png.c', 'png-fnott.h',
  'shm.c', 'shm.h',
  'svg.c', 'svg.h',
  'stride.h',
  'wayland.c', 'wayland.h',
  'xdg.c', 'xdg.h',
  wl_proto_src + wl_proto_headers, version,
  dependencies: [threads, pixman, png, wayland_client, wayland_cursor, dbus,
                tllist, fcft, nanosvg],
  install: true)

executable(
  'fnottctl',
  'fnottctl.c',
  'ctrl-protocol.h',
  'log.c', 'log.h',
  version,
  install: true)

install_data('fnott.desktop', install_dir: join_paths(get_option('datadir'), 'applications'))
install_data('fnottrc', install_dir: join_paths(get_option('datadir'), 'fnott'))

subdir('completions')
subdir('doc')
