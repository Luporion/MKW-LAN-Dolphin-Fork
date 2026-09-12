# MKW LAN v0.9-Luporion.2

Modified by Luporion, based on the LAN Multiplayer mod by MrBean35000vr and
Chadderz and the BrainSlug integration maintained by calvinhendriks.

This release enables multiple Dolphin instances on one PC using separate
control ports and peer PIDs. It adds automatic game-port fallback when another
application or a Windows reservation blocks the game's requested port, fixes
control-socket cleanup, and identifies the fork in the loader and in-game credits.

Validated on Windows with Dolphin 2606a and vanilla PAL RMCP01:

- Two-instance racing, including split-screen with four players.
- Four instances discovering each other, joining one room and racing.
- Two independent two-instance rooms with distinct group IDs.
- Reconnection after an AFK disconnect without restarting emulation.
- In-game recovery from a blocked game port using automatic port assignment.
- Separate User/SD directories, including migration of the setup to another drive.

Real Wii hardware, six/twelve simultaneous instances and prolonged independent
two-group racing are not yet validated. AFK disconnection itself is unchanged.
All devices in a mixed setup should use this modified module; interoperability
with the original fixed-port module is not guaranteed.

Download the module ZIP and copy `bslug/modules/rmc-local-net.mod` into each
existing BrainSlug SD setup with Dolphin closed. This is a module-only release.
Game images, NANDs, saves and the legacy custom-track bundle are not included.

See `docs/DOLPHIN-SETUP.md` for installation/build instructions and
`docs/TESTING.md` for the full evidence and limitations. Original credits and
license notices are retained.
