# AMG65 Display Bundle

This bundle is a custom animation injection chain for the dot-matrix display on the LEOBOG AMG65 keyboard.

It works by reusing the keyboard driver's display preview path and sending animation data directly to the screen HID interface.

The main problem this repository solves is that the stock driver does not support true runtime-randomized animation and also imposes practical frame-count limits on imported animation content.

This injection-based approach removes those two limits for custom runtime playback, with the tradeoff that animation speed is constrained by the stability limits of the real-time preview transport path.

An example runtime randomized animation based on Needy Girl Overdose-style pixel art is included in this repository.

Pixel art source reference:

- https://www.pixiv.net/users/50998597

For anyone building custom content on top of this chain: the JSON files in `animation_segments\` were edited and exported through the official AMG65 driver.

In the current C implementation, those JSON files are used mainly as sources for frame order and per-frame pixel content. Driver-side playback metadata such as exported speed values is not used directly by this runtime.

## Files

- `display_start.exe`
- `display_stop.exe`
- `animation_segments\`
- `src\`

## Usage

- `display_start.exe [delay_ms]`
- `delay_ms` is optional.
- Unit: milliseconds per packet.
- If omitted, the built-in default is used.
- `display_stop.exe` takes no arguments.

Start:

```powershell
.\display_start.exe
```

Start with a custom delay:

```powershell
.\display_start.exe 10
```

Stop:

```powershell
.\display_stop.exe
```

## Recommended Delay

- `10 ms`
- This is the current daily-use setting.
- On the current test setup, this is around the fastest stable value for long-running playback.
- Lower values can introduce visible packet/frame corruption.
- If you want substantially faster animation playback, this real-time injection path is probably not the right approach.

## Behavior

- `display_start.exe` is windowless and background-oriented.
- A named mutex prevents duplicate start instances.
- If the keyboard is not present when started, the daemon keeps retrying.
- If the keyboard is unplugged and replugged later, the daemon attempts to reconnect automatically.
- `display_stop.exe` signals the running instance to exit cleanly.

## Auto Start

- Use Windows Task Scheduler.
- Trigger: `At log on`
- Delay: `10 seconds`
- Program: `display_start.exe`
- Optional argument: `10`

## Required Layout

- Keep these items together in one folder:
  - `display_start.exe`
  - `display_stop.exe`
  - `animation_segments\`
- `display_start.exe` resolves assets via relative path:
  - `.\animation_segments\*.json`

## Developer Files

- `src\display_start.c`
- `src\display_stop.c`
- `src\hid_session_sender.c`
