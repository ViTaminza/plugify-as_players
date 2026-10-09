# as_players (Plugify)

Admin player-action module for admin_system (port of the Pisex Metamod:Source module 'Players'): mm_kill, mm_kick, mm_slap, mm_rename, mm_changeteam, mm_switchteam, mm_noclip and mm_who, plus the 'players' admin-menu category with the eight matching items (who, noclip, switchteam, changeteam, rename, slap, kick, kill) and the player/team/damage picker menus. Matches the original commands, permissions, phrases and action parameters sent to the admin core. No exported methods.

Plugify port (fork) of **Players** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_players-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_players`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

