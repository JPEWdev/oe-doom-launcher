project('oe-doom-launcher', 'c')

avahi_client_dep = dependency('avahi-client')
avahi_glib_dep = dependency('avahi-glib')
glib_dep = dependency('glib-2.0')
libbsd_dep = dependency('libbsd-overlay')
systemd_dep = dependency('libsystemd')
udev_dep = dependency('libudev')

executable('oe-doom-launcher', [
    'src/main.c',
  ],
  dependencies: [
    avahi_client_dep,
    avahi_glib_dep,
    glib_dep,
    libbsd_dep,
    systemd_dep,
    udev_dep,
  ],
  install: true
)
