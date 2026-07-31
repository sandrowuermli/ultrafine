# ultrafine

Set the brightness of LG UltraFine displays on macOS **by Thunderbolt port** instead of relying on macOS's display↔control-device pairing.

If you run two identical LG UltraFine monitors and the brightness sliders drive the wrong panel — or both drive the same one, or one is grayed out — this fixes it.

## Why I built this

I have two LG UltraFine 5K monitors. Same model, same firmware. For years, adjusting brightness was a coin flip:

- sometimes both sliders moved the same physical monitor,
- sometimes each slider moved the *other* monitor,
- sometimes a slider was grayed out entirely,
- and sometimes it just worked.

My workaround was unplugging and replugging monitors until the sliders behaved. Sometimes one replug was enough, sometimes I had to unplug both and start over. Pure luck.

### This has been broken since day one

Not a local quirk, and not new. The UltraFine 5K arrived at the end of 2016 as
Apple's official Retina display for the first Thunderbolt 3 MacBook Pro, and
brightness control on it has been failing in public ever since:

- **January 2017** — the brightness slider for the LG 5K is missing entirely.
  The Apple Support thread runs for more than a year, collects NVRAM-reset
  folklore, gets no answer from Apple, and is closed by the system.
  ([thread](https://discussions.apple.com/thread/7827966))
- **February 2020** — two UltraFine 5Ks on Catalina: "the brightness adjustment
  icon appears to show it reducing on the correct screen but the actual
  brightness changes on the other one." The best advice on offer is downgrading
  to High Sierra, "the last stable OS with external monitors."
  ([thread](https://forums.macrumors.com/threads/problem-w-dual-lg-5k-monitors.2223426/))
- **January 2022** — reported against MonitorControl on an M1 Max with 2×
  UltraFine 5K. Closed as out of scope, because the display is not on DDC:
  "the UltraFine 5K is controlled natively by macOS (MC also uses Apple's APIs
  to control the display)." The usual third-party brightness tools cannot reach
  this pairing at all.
  ([discussion](https://github.com/MonitorControl/MonitorControl/discussions/874))
- **March 2024** — slider greyed out on an M2 Max, while a 2017 MacBook Air
  drives the same panel without trouble. Thread closed with zero replies.
  ([thread](https://discussions.apple.com/thread/255545858))

Nine and a half years, Intel and Apple Silicon, every macOS release from Sierra
onward. No fix has shipped from either side, and none of those reports ever got
an official answer. The remedies that circulate — reset the NVRAM, replug the
Thunderbolt cable, move USB peripherals off the monitor's hub, reboot with one
display disconnected — do not fix anything. They only re-roll the guess
described below, which is why they work once and then stop.

So I went looking for the actual reason myself.

### The cause

Brightness on an UltraFine does not travel over DDC. The monitor exposes a USB HID device (`USB Controls` → `HID BRIGHTNESS`) and macOS writes a feature report to it.

The problem is what that device looks like to the operating system:

| Property | Monitor A | Monitor B |
| --- | --- | --- |
| `idVendor` | 1086 | 1086 |
| `idProduct` | 39488 | 39488 |
| `iSerialNumber` | **0** | **0** |
| Report descriptor | identical | identical |

**The brightness device carries no serial number.** With two identical monitors, macOS sees two identical, anonymous control devices and has to guess which one belongs to which display. That guess comes out of enumeration order at connect time, so every replug re-rolls it. Four outcomes are possible: correct, swapped, collided (both sliders on one panel), or unpaired (slider disabled).

I confirmed the collided case in the IORegistry. `WindowServer` had opened **two** HID clients on one monitor's brightness endpoint and **none** on the other's, and the unused endpoint had received zero brightness writes since boot:

```
endpoint A   WindowServer clients: 2   SetReportCount: 85
endpoint B   WindowServer clients: 0   SetReportCount: 0
```

Writing to the ignored endpoint by hand dimmed that panel immediately — so the monitor, cable and USB path were all fine. macOS simply never addressed it.

### The fix

There *is* a stable identifier: the USB `locationID`, which is tied to the physical Thunderbolt port. macOS doesn't use it for this pairing. This tool does.

## How it works

1. Finds every LG brightness endpoint via `IOHIDManager`, matching vendor `1086` with HID usage page `0x80` (Monitor) and usage `1`.
2. Identifies each one by `kIOHIDLocationIDKey` — stable for as long as the monitor stays in the same port.
3. Reads and writes a 6-byte HID **feature report**. The first four bytes are the brightness in nits × 100, little-endian, ranging from `400` (4 nits) to `54000` (540 nits).
4. Maps your labels (`left`, `right`, …) to location IDs through `~/.config/ultrafine/map.conf`.

Percentages are converted to nits with a gamma of 1.8, which approximates Apple's own perceptual curve closely enough that 100% matches what the system slider produces. Use `Nnits` when you want an exact value.

No entitlements, no kext, no daemon. It is a single binary that talks to the monitor and exits.

## Install

Requires macOS on Apple Silicon and the Xcode Command Line Tools.

```sh
git clone https://github.com/sandrowuermli/ultrafine.git
cd ultrafine
./build.sh
```

`build.sh` compiles a native arm64 binary, ad-hoc signs it, and installs it to `~/.local/bin/ultrafine`. Make sure that directory is on your `PATH`.

Pass a file or a directory to build somewhere else — `./build.sh ../vendor/bin/ultrafine` — which is what a project that vendors this repo as a submodule does.

`ultrafine` and `ultrafine both` work immediately, with no setup. To address a single monitor by name, label them once:

```sh
ultrafine identify
```

It dims each monitor in turn and asks you to name the one that just dimmed. Your labels are written to `~/.config/ultrafine/map.conf`, one line per port:

```
0x00100000 right
0x00200000 left
```

Nothing is hardcoded — the location IDs are discovered on your machine and depend on which ports you use, so yours will look different from anyone else's. Until you run `identify`, monitors are simply listed by their location ID.

Re-run `identify` if you ever plug a monitor into a different port; labels follow ports, not panels.

## Usage

```
ultrafine                  show every monitor and its current brightness
ultrafine both             set every connected monitor to 100%
ultrafine left             set the monitor labeled "left" to 100%
ultrafine right            set the monitor labeled "right" to 100%
ultrafine both 60          set every connected monitor to 60%
ultrafine left 30          set one monitor to 30%
ultrafine right 250nits    set an absolute nits value
ultrafine 0x00200000 60    address a monitor by its location ID
ultrafine identify         (re)assign labels
ultrafine help             print usage (also -h and --help)
```

Omitting the value means 100%. `help` works with no monitors connected. Labels are free text — name them `desk`, `vertical`, `a` and `b`, whatever fits your setup.

Status output:

```
$ ultrafine
right      port 0x00100000  100%  (540 nits)
left       port 0x00200000   30%  (65 nits)
```

## Notes

- Works regardless of how macOS paired the displays — including when Control Center's slider is grayed out.
- It writes brightness directly to the panel, so the macOS slider position may no longer reflect reality until you touch it again.
- Developed against the UltraFine 5K. Other UltraFine models expose the same HID interface and should work; the tool matches on vendor and usage page, not on a product ID.
- One monitor, three monitors, mixed models: all fine. Every connected LG brightness endpoint is discovered at runtime.
- It writes a HID feature report straight to the display's brightness endpoint — the same report macOS itself writes, clamped to the panel's 4–540 nit range. It touches nothing else and installs nothing persistent. Still, it addresses your hardware directly: use it at your own risk, without warranty of any kind (see [LICENSE](LICENSE)).

## Trademarks

LG, UltraFine and LG UltraFine are trademarks or registered trademarks of
LG Electronics Inc. Apple, Mac, macOS, MacBook Pro, Apple Silicon and Xcode are
trademarks of Apple Inc. Thunderbolt is a trademark of Intel Corporation.

This is an independent, unofficial tool. It is not affiliated with, endorsed by,
sponsored by or supported by LG Electronics, Apple or Intel. Those names are
used here only to identify the hardware and software this tool works with.

## License

MIT — see [LICENSE](LICENSE).
