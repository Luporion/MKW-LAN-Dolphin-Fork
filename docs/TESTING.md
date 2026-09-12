# Validation record — v0.9-Luporion.2

Test date: 2026-09-12. Tests performed by Luporion on one Windows PC using
Dolphin 2606a, JIT64, Direct3D 11 and the PAL game RMCP01. Each emulator used
a separate User directory and SD sync directory. This is a manual functional
test record, not a performance benchmark or a guarantee of indefinite uptime.

## Confirmed results

| Scenario | Evidence and result |
| --- | --- |
| Two Dolphin instances on one PC | Discovery, room creation/join, completed DWC/GT2 matching and racing confirmed in logs and screenshots. |
| Two local players per instance | Earlier screenshots show two split-screen instances in a four-player race. |
| Four simultaneous instances, one player each | Four peers discover one another, join the same room and race. Screenshots show all four clients in the room and race; logs share group 1797995097. |
| Two independent two-console rooms | Screenshots show both rooms simultaneously. SD1/SD2 match group 1798720999; SD3/SD4 match group 1699669702 and later 1704120390. Separate hosting and joining confirmed. |
| Rejoining after an AFK-related disconnect | User explicitly confirmed successful reconnection without restarting emulation after installing Luporion.2. Logs show fresh network initialization and matching. This does not disable AFK disconnection. |
| Game-port fallback in Dolphin | SD4 logs requested port 54515 failing with -2, automatic assignment of port 60941, then completed matching in group 1699669702. |
| Credits and loader identification | Screenshots confirm Luporion version identification and modified-version credits while preserving MrBean35000vr, Chadderz and the original roomkey.com text. |
| Moving the setup to another drive | User/SD/Launcher setup moved from C: to E:, configuration paths updated, followed by successful tests. No rebuild required for relocation. |
| Six profiles prepared | User1–User6 and SD1–SD6 created; screenshot confirms identical module hashes. This is preparation only, not a six-instance gameplay test. |

The four supplied current SD modules have SHA-256:

```text
8e53d839f18c336821477bae3644f741d42ac81562fb479b9826d80b27490f05
```

Logs contain appended sessions from older builds. Older GT2 errors must not
be attributed to the final build without checking the session and log lines.
Raw logs and full-desktop screenshots are not included in this public record.

## Scope still requiring testing

- Six simultaneous emulator instances; twelve instances with one player each.
- Twelve players using a mixture of single-player and split-screen instances.
- Real Wii consoles running this modified module, including mixed Wii/Dolphin rooms.
- USA/Japan builds and custom track combinations with this fork.
- Two groups racing on different tracks simultaneously for an extended period,
  and one group disconnecting/restarting while the other continues racing.
  Current evidence establishes separate rooms; it does not establish this entire stress test.
- Multiple physical PCs, different LAN adapters, and long sessions with repeated joins.
- Complete OS socket/port exhaustion and final-error UI behavior.

## Reproduced failures and fixes

MSI.CentralServer previously owned 7,320 UDP endpoints, including the exact
three game ports that failed to initialize. Independent Windows bind tests
confirmed those collisions. The reason MSI retained these endpoints was not
established; this fork does not modify or terminate MSI software.

After MSI was stopped, reconnecting could still fail because a newly chosen
game port fell in a Windows excluded UDP range. Port 58118 was in 58082–58181;
54460 was in 54451–54550. Independent bind tests returned access denied.

Luporion.2 retries a failed game-socket bind with port zero on the same socket,
within the LAN network thread's GT2 initialization only. The network stack
selects and binds an available port atomically. The actual bound port is read
back and advertised. DWC initialization is not blindly repeated after failure.

## Automated/build checks

- Complete devkitPPC compile and link; original and modified builds share the
  existing assembler-comment and GNU-stack warnings.
- BMG checked for 34 message IDs, correct section alignment/offsets and retained
  original credits; other messages and embedded control sequences unchanged.
- Broadcast packet declaration unchanged from the supplied working baseline.
- Prior PID/cleanup harness: distinct local PIDs for 30 port offsets and cleanup
  on normal exit and partial initialization failure.
- Actual bind-hook C function tested with GCC across ten cases: success;
  failed bind; fallback failure; calls outside initialization, on another thread,
  or on discovery socket; port zero; other address family; NULL input. At most
  two binds, caller address unchanged, final errors propagated.
- Native Windows tests: occupied port and excluded port 58118 both recover by
  binding port zero on the same socket; twelve simultaneous test sockets receive
  twelve distinct ports. These socket tests are not twelve-player gameplay tests.

## Repeatable manual test

1. Start two instances with separate User/SD folders; verify loader version.
2. Join a room and race. Leave LAN on both and rejoin without restarting emulation.
3. Repeat after a disconnect; check `game socket ready on port ...` and matching.
4. Start four instances; test one four-client room, then two separate pairs.
5. Run different tracks concurrently, restart only one group, and record whether
   the other group continues unaffected. This is the next isolation stress test.
6. Increase to six and twelve only after the smaller tests pass.

Keep logs per instance and note the exact build, player count, host/client roles,
number of races, deliberate exits and unexpected failures.
