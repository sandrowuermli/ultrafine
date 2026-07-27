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

So I went looking for the actual reason.

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
```

Omitting the value means 100%. Labels are free text — name them `desk`, `vertical`, `a` and `b`, whatever fits your setup.

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
