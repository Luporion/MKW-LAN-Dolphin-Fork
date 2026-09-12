# Dolphin setup for the Luporion fork

Use a legally obtained game image and an existing working BrainSlug setup.
The tested base is vanilla PAL Mario Kart Wii (RMCP01). This release contains
the LAN module only, not a game image, NAND, saved profile, or full SD bundle.

## Directory layout

Choose a drive with enough free space, for example:

```text
E:\Wii\MKW-LAN-Test\
  Launcher\brainslug-boot.dol
  User1\
  User2\
  SD1\bslug\modules\rmc-local-net.mod
  SD2\bslug\modules\rmc-local-net.mod
```

Each instance needs its own User directory, SD sync folder and virtual SD image.
The launcher DOL can be shared; keep it outside the synchronized SD directories.
The game image can also be shared as the configured Default ISO.

In each Dolphin profile enable SD insertion, writes and folder synchronization.
Point its SD sync folder to SD1, SD2, etc. Use distinct controller assignments.
Duplicated profiles also copy controller mappings and Miis; give the hosts
different recognizable Miis if you want to distinguish rooms easily.

The tested minimal module set is:

```text
console-sd.mod
libfat-sd.mod
libfat.mod
libsd.mod
rmc-local-net.mod
```

Keep the matching BrainSlug symbols. The repository's legacy root_SD includes
custom-track components and is not the tested vanilla minimal setup. Replace
its LAN module if using it, but do not assume its complete configuration has
been validated with this release.

Close all instances before replacing modules, copying profiles, or moving SDs.
Install the release module at `bslug/modules/rmc-local-net.mod` on each SD.

## Launch

PowerShell (adjust Dolphin path as needed):

```powershell
$Dolphin = 'E:\Wii\Dolphin-x64\Dolphin.exe'
$Root = 'E:\Wii\MKW-LAN-Test'
foreach ($N in 1..2) {
    Start-Process -FilePath $Dolphin -ArgumentList "-u `"$Root\User$N`""
    Start-Sleep -Seconds 2
}
```

Open `Launcher\brainslug-boot.dol` in each window. Verify
`v0.9-Luporion.2` on the loader. For four prepared profiles use `1..4`.
To add two more later launch only `5..6`; do not reopen profiles already running.

## Rooms and limits

Create a room in one instance and join its Mii from another. For two groups,
create two hosts and have clients select the appropriate host. The common LAN
list shows both groups; there is no password or hidden-group filter.

The game limit remains twelve players, counting local second players. Discovery
uses 30 friend slots and control ports 27900–27929. These are different limits.
Four emulator instances are gameplay-tested; six profiles are prepared and
twelve instances remain a target to validate.

For real Wiis use the same modified module on all devices. The original module's
fixed control port/IP identity does not reliably handle several Dolphins sharing
one IP. Compatibility with Wii hardware is intended, but not yet hardware-tested.

## Troubleshooting

- `game port ... bind failed ... requesting a free port` followed by
  `game socket ready on port ...` means fallback succeeded, not a fatal error.
- The older DWC `Private port` log can show the unsuccessful requested port;
  `game socket ready` is the actual port used.
- Software holding thousands of sockets can still exhaust OS resources. This
  module cannot guarantee recovery if no sockets/ports remain available.
- Allow Dolphin on the intended LAN in the firewall; game traffic also uses
  dynamically assigned UDP ports. Do not delete Windows port reservations.
- After an AFK disconnect, leave LAN on both sides and enter again. AFK kicking
  itself has not been removed.
- Old peers reserve friend slots until LAN is exited. After many full restarts
  with new PIDs, leave/re-enter LAN on surviving instances to clear the list.
- Final initialization failure still has the old imperfect menu handling;
  inspect the log if the LAN menu appears but nobody can be discovered.
- If moving drives, close Dolphin, move the whole layout and update absolute
  User/SD paths in the profile configurations. No module rebuild is needed.

## Build

Inside the existing WSL/devkitPro environment:

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC="$DEVKITPRO/devkitPPC"
export PATH="$DEVKITPRO/tools/bin:$PATH"
cd src/brainslug-wii-vanilla/modules/rmc-local-net
make clean
make
```

BrainSlug SDK/headers and linker scripts must already be installed under
`$DEVKITPRO/bslug`. Output: `bin/rmc-local-net.mod`. Normal header dependencies
are incomplete in the inherited Makefile, so use a clean build for releases.

From repository root, `python3 tests/test_port_fallback.py` runs the host C tests
(GCC required). On Windows, `python tests/test_port_fallback.py --windows` runs
native socket tests; the excluded-port test is specific to the recorded machine
and skips if port 58118 is no longer excluded.
