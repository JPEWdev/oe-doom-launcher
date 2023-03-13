# OpenEmbedded Demo Doom Launcher

This project implements a simple systemd service that manages the OpenEmbedded
Booth Doom Demo. The service uses Avahi to advertise clients that can join a
multiplayer Doom game, then one of the devices will be selected to be a host
for all the other clients.

If no other clients are detected to join a multiplayer game the demo will run a
single player game instead. If a client leaves a multiplayer game, it will also
go back to running a single player game until the next time a host starts a
multiplayer game.

The daemon also automatically restarts the game if it exits for any reason

# Configuration

The daemon takes a ini config file which defaults to
`/etc/oe-zdoom/config.ini`, but can also be specified on the command line with
the `--config` option. The ini file has the following options (with their defaults):

```ini
[global]
# The zdoom program to run (either in $PATH, or the fullly qualified path)
zdoom = zdoom

[multiplayer]
# The WAD file to use when hosting a multiplayer game
wad = freedm.wad

# The multiplayer map to use when hosting a multiplayer game
map = MAP01

# Is this device eligible to be a multiplayer host?
can-host = true

# Which port to use for hosting the multiplayer game
port = 5029

# How long to wait (in seconds) after a new game client is seen before hosting
# a game
wait = 30

# The -config parameter to pass to zdoom when launching a multiplayer game
# (default is to not specify a -config argument)
#config =

[singleplayer]
# The WAD file to use when running a single player game
wad = freedoom1.wad

# The -config parameter to pass to zdoom when launching a single player game #
(default is to not specify a -config argument)
#config =
```

# Hosting Preference

When advertising as a client, each device also advertises its preference to
host a game in the mDNS record. A value of 0 for the preference means that this
device cannot host a game, otherwise the host with the highest preference is
selected to host. If a tie occurs, the host machine ID is used to ensure a
stable tie breaker. The daemon will automatically set the host preference based
on the following rules:
1. The value of the command line `--hot-preference` argument. This is primarily
   useful for testing the daemon, or if you want to host from a PC.
2. If `multiplayer.can-host` in the config file is `false`, the host
   preference is `0` (this device will never host)
2. If the device has a USB keyboard plugged in, the host preference is `2`.
   This makes it easier to change host settings during a demo (such as changing
   the map), by ensuring that if there is only one device with a USB keyboard
   plugged in, it will always be the host.
3. The host preference is `1`
