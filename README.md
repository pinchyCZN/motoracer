# MotoRacer - Reverse Engineering and Modernization

A from-source reconstruction of **MotoRacer** (Delphine Software / Electronic
Arts, PC, 1997), rebuilt as a modern 32-bit Windows executable.

The goal is a **drop-in `.exe`**: copy it into an existing MotoRacer
installation and run. No patched data, no pre-extracted assets, no
converted textures. Everything is read out of the game's own `.BKF`
archives and `.BPT` records exactly where a stock install puts them.

> This is a preservation and interoperability project built against a
> legally owned copy. **No game data is included in this repository** and
> none is redistributed. You need your own installation to run it.

---

## What has been replaced

The original is hard-wired to APIs that no longer exist in a shipping
Windows SDK, so those parts are replaced rather than translated:

| Subsystem | Original | Here |
|---|---|---|
| Rendering | DirectDraw + a software rasterizer; optional Direct3D execute buffers | **Direct3D 9** |
| Colour depth | hard-locked to 16-bit; refuses to start otherwise | any depth, 32-bit by default |
| Audio | DirectSound | **XAudio2** 2.9 / 2.8, with a 2.7 fallback |
| Input | `GetKeyState` + `winmm` joystick | Win32 keyboard (joystick not yet reconnected) |
| Networking | DirectPlay | **TCP relay** (see [Netplay](#netplay)) |
| Config | `.ini` + registry | `.ini` only |

Everything else - physics, the drivetrain, the AI, the rider animation,
the track format, the menus - is transcribed from the original binary
rather than reimagined, and is verified against it where possible. The
`MR_Racer` record is byte-exact with the original's layout so the same
memory offset means the same field in both programs, which is what makes
side-by-side comparison possible.

## Project status

Playable. Solo racing, time attack, the menu flow, audio, the HUD and
network play all work. It is not finished: see `docs/` for what is
understood and what is not, and for the open questions in each subsystem.

---

## Requirements

**To build**

- Visual Studio 2017 (toolset **v141**) with the C++ desktop workload
- Windows 10 SDK **10.0.17134.0**
- No DirectX SDK required. The XAudio2 2.7 interface is transcribed into
  `src/xa2.cpp`, so the Windows SDK alone is enough.

**To run**

- Windows 8 or later, or Windows 7 with the DirectX End-User Runtime
  (June 2010) installed for `XAudio2_7.dll`
- A Direct3D 9 capable GPU
- An existing MotoRacer installation for the `DATA` folder

The Release build links the CRT statically (`/MT`), so no Visual C++
redistributable is needed.

## Building

Open `src/mracer.sln` in Visual Studio and build, or from a developer
command prompt:

```
msbuild src\mracer.sln /p:Configuration=Release /p:Platform=Win32
msbuild src\mracer.sln /p:Configuration=Debug   /p:Platform=Win32
```

**Win32 only.** The reconstruction is 32-bit: it inherits the original's
pointer-width assumptions and its structure layouts are pinned to the
original's. There is no x64 configuration.

Output lands in `build\Win32\<Configuration>\mracer.exe`.

## Running

Put `mracer.exe` beside a MotoRacer installation, or point it at one.
Configuration lives in `mracer.ini` next to the executable:

```ini
[PATHS]
datadir=.\DATA\

[PLAYER]
name=PLAYER

[VIDEO]
maxfps=0

[DEBUG]
logfile=1
```

`datadir` is resolved relative to the executable, so `.\DATA\` means the
`DATA` folder beside it. `[DEBUG] logfile=1` writes `mracer.log` next to
the executable, which is the first thing to read when something is wrong.

---

## Controls

### Riding

| Key | Action |
|---|---|
| `Z` | Accelerate |
| `X` | Brake |
| `Left` / `Right` | Steer |
| `C` | Stunt / wheelie |
| `B` / `V` | Higher / lower gear (manual gearbox only) |

### Camera and view

| Key | Action | |
|---|---|---|
| `5` | Change view - cycles four chase distances | retail's `F5` |
| `6` | First person | retail's `F6` |
| `7` | Rear view, **held** not toggled | retail's `TAB` |
| `1` | Side view | this port only |
| `F9` | Cycle the map display (five modes, one of them off) | |
| `F4` | Toggle 16:9 | retail's key |

### Menus and game

| Key | Action |
|---|---|
| `Enter` | Confirm / select |
| `Escape` | Skip the intro; back / cancel / accept, depending on the page |
| `Escape` or `Q` | In-race: open the race menu (continue / restart / quit) |
| `M` | Mute / unmute |
| `F` | Borderless fullscreen |
| `F6` | Toggle texture filtering (this session only) |
| `Ctrl` + `V` | Paste - only into the relay address field |

Audio mutes automatically when the window loses focus, and keyboard input
is ignored while another window has focus.

### Debug and harness

These exist for development. They are harmless but they are not part of
the game.

| Key | Action |
|---|---|
| `Space` | Pause / resume (disabled during a network race) |
| `F5` | Step one frame, pausing first |
| `Tab` | Fast-forward while held |
| `2` | Restart the current menu page |
| `3` | Guard rails on / off - ride through the walls |
| `F8` | Toggle the autopilot |
| `[` / `]` | Lower / raise the drawn-primitive limit |
| `\` | Clear the primitive limit |

---

## Netplay

The original used DirectPlay, which is long gone. This port replaces it
with a small **TCP relay**: a Python script that both players connect out
to, so neither needs to accept an inbound connection or forward a port.
Up to **eight** players per room.

### 1. Run the relay

Anywhere both players can reach - a VPS, or one player's machine on a LAN.
Python 3 and no dependencies:

```
python tools/relay.py                      # listens on 0.0.0.0:27500
python tools/relay.py --port 27600
python tools/relay.py --bind 203.0.113.5   # one interface only
python tools/relay.py --verbose
```

`tools/mracer-relay.service` is a systemd unit for running it on a Linux
server. `tools/README-relay.txt` has the details, including how to limit
access to a single address with a firewall rule.

### 2. Connect

From the main menu:

1. **LINK** - goes to the connect page
2. Type the relay address as `host` or `host:port` - port 27500 is assumed
   if you leave it off. `Ctrl+V` pastes here, which is the only place it
   works.
3. **OK** - the game list appears once you are on the relay

### 3. Host or join

On the game list page:

- **CREATE** makes a room listed under your player name
- Selecting a listed room joins it
- **BACK** disconnects and returns to the connect page

Rooms are listed by host name; there is no password or room code.

### 4. The lobby

Everyone who joins lands in the participants page, which carries the
roster, a three-line chat, the track photo, the lap counter and the bike
selection. The **host decides** the track, the lap count, the mode, and
whether pocket bike and reverse mode are on; guests see those settings
arrive and cannot change them. Each player picks their own bike.

The host starts the race for everyone.

### Notes and limits

- **There is no pause in a network race.** The other riders do not stop,
  so neither can you. The race menu opens as an overlay instead.
- Finishing shows your result immediately; the host converges stragglers.
- A guest's pocket and reverse settings are restored when they leave the
  network pages, so a host's choices do not follow them into solo play.
- Track availability gates **solo** selection only, so a network game can
  race the islands that are locked in single player.
- Difficulty is not yet host-authoritative: each player races their own
  time limit.

---

## Repository layout

```
src/        the reconstruction. mracer.sln, MSVC 2017, no precompiled header
docs/       what is known about the binary - formats, structs, subsystems
tools/      the relay, asset decoders, Ghidra scripts, A/B harnesses
build/       compiler output                                   [not committed]
decomp/      Ghidra project and the game binary                [not committed]
```

`docs/` is written as plain-text RFC-style notes rather than markdown, and
every claim in it carries an evidence tag - `[verified]` read out of the
binary, `[inferred]` reasoned from it, `[observed]` seen in a recording,
`[guess]` untested. `README.md` is the one markdown file in the tree,
because GitHub renders it.

Start with `src/README.txt` for how the source is organised, and
`docs/README.txt` for the documentation conventions.

---

## Legal

MotoRacer is the property of its respective rights holders. This project
contains **no game code and no game assets** - only original work:
reconstructed source, documentation of the file formats, and tooling. The
game binary and its data are excluded from version control and are not
redistributed.

You need a legally obtained copy of MotoRacer to use any of this.
