# pisugar3-battery

A Linux kernel driver for the [PiSugar 3](https://www.pisugar.com/) family of
Raspberry Pi UPS boards - PiSugar 3, 3 Plus and 3 Air - that puts the battery
where Linux expects one: `/sys/class/power_supply`. Anything that reads a
laptop battery reads this one, unchanged: `upower`, desktop panels, `acpi`-style
scripts, `cat /sys/class/power_supply/pisugar3-battery/capacity`.

```
$ ls /sys/class/power_supply/
pisugar3-battery  pisugar3-mains
$ cat /sys/class/power_supply/pisugar3-battery/{status,capacity,voltage_now,temp}
Discharging
63
3748000
400
$ upower -i /org/freedesktop/UPower/devices/battery_pisugar3_battery
  ...
  state:               discharging
  percentage:          63%
  voltage:             3.748 V
  temperature:         40 degrees C
  technology:          lithium-polymer
```

## What the board can and cannot tell you

The PiSugar 3 is a small microcontroller on I2C (address `0x57`) in front of a
charger, a boost converter and a single-cell LiPo. It reports:

- the cell voltage (`0x22`/`0x23`, millivolts),
- whether external power is present and whether charging is enabled (`0x02`),
- its own temperature (`0x04`),
- a percentage of its own (`0x2A`).

It does **not** measure current. The current registers exist in the register
map and read zero on every firmware seen so far (v1.3.4 included), so there is
no coulomb counting and no time-to-empty from the hardware. The driver is a
voltage gauge, and it is honest about that: no `current_now`, no
`time_to_empty`, no invented `charge_now`.

**Capacity comes from a table, and the table is yours.** The driver looks up
the voltage in the `ocv-capacity-table-0` of the `monitored-battery` node in
the device tree. Only when there is none does it fall back to the board's own
percentage, which is a generous lookup rather than a measurement: on a 5000 mAh
cell it read 64 % at 3.70 V with a third of the runtime left, and 8 % at the
point the device had to shut down.

**The driver never writes.** The board carries one register bit whose loss is
not recoverable in software - "turn the output back on when external power
returns", which on a device with no power button is the only way it ever
comes back from a flat battery - and a read-only driver cannot lose it.

## Install on Raspberry Pi OS

```bash
sudo apt install -y build-essential linux-headers-$(uname -r) device-tree-compiler
git clone https://github.com/lucamartinetti/pisugar3-battery
cd pisugar3-battery
make
sudo make install
```

Then pick an overlay and add it to `/boot/firmware/config.txt`:

```
dtoverlay=pisugar3                 # PiSugar 3 / 3 Plus, the vendor's default curve
dtoverlay=pisugar3-air-5000mah     # a 3 Air with a 5000 mAh cell, a measured curve
dtoverlay=pisugar3,addr=0x5a       # a board whose I2C address was changed
```

`dtparam=i2c_arm=on` has to be there too. Reboot, or load it now:

```bash
sudo dtoverlay pisugar3
ls /sys/class/power_supply/
dmesg | grep pisugar
```

`sudo make uninstall` removes the module and the overlays; take the
`dtoverlay=` line out yourself.

### Cross-building from a workstation

Building on a Pi Zero takes minutes; cross-building takes seconds.
`tools/deploy.sh` fetches the headers for whatever kernel the Pi is running
from the Raspberry Pi apt archive, builds the module and the overlays, installs
them over SSH, and optionally loads an overlay:

```bash
tools/deploy.sh pi@raspberrypi                       # install only
tools/deploy.sh pi@raspberrypi pisugar3              # ...and load the overlay now
tools/deploy.sh pi@raspberrypi pisugar3 --persist    # ...and put it in config.txt
```

You need `aarch64-linux-gnu-gcc`, `dtc`, `bsdtar`, and - because the Debian
headers package ships its kbuild helper binaries as arm64 executables - a
`binfmt_misc` registration for aarch64 plus `QEMU_LD_PREFIX` pointing at an
arm64 sysroot with a libc in it. Passwordless `sudo` on the Pi.

## Measuring a curve for your cell

The vendor's default table (`overlays/pisugar3.dts`) is a voltage lookup for
a generic cell, and it reads high through the middle of a discharge on a large
one. A percentage that means something needs a table measured on the cell that
is fitted, at the load the device actually draws. It is a one-evening job:

1. Charge fully. Unplug. Log the voltage once a minute until the device shuts
   down - `cat /sys/class/power_supply/pisugar3-battery/voltage_now` in a loop
   with a timestamp, to a file that survives the shutdown.
2. For each 25 mV band of voltage, take the median of *time remaining until
   shutdown* over the minutes spent in that band, and scale it: the first
   minute is your top percentage, the shutdown is your bottom one.
3. Write the pairs, microvolts first and descending, into
   `ocv-capacity-table-0` of a copy of the overlay. Set
   `charge-full-design-microamp-hours` to the cell's rating.

That is what `overlays/pisugar3-air-5000mah.dts` is. It makes a percent a
fixed slice of runtime at that load, which is what a person reads a percentage
as, and the straight line that any time-to-empty estimate wants. The kernel
calls the property an open-circuit-voltage table and it is not one - it is a
loaded discharge curve - but the lookup is the same, and a loaded curve is the
one that is true while the device is on. A heavier load than the one measured
reads *low* on it, which is the safe direction to be wrong in.

## Sharing the board with userspace

Once the driver has bound to address `0x57`, `i2c-dev` refuses that address to
userspace: `i2cget`, and any program that opens the bus and claims the
address afterwards, get `EBUSY`. That is the kernel protecting the device from
two masters, not a bug. A program that already held the address when the
driver loaded keeps working, which is how a daemon can survive the overlay
being loaded under it and then fail after its next restart. `i2cget -f`
forces the claim for a one-off look at a register; a program that used to
read the board itself should read `/sys/class/power_supply` instead while the
driver is loaded, and that is the point of having one. The RTC at `0x68` is a
separate address and is not affected.

## What is exposed

`pisugar3-battery` (`POWER_SUPPLY_TYPE_BATTERY`):

| property | source |
|---|---|
| `status` | Discharging; Not charging when plugged in with charging disabled; Charging; Full at the top of the table |
| `present` | always 1 |
| `technology` | Li-poly |
| `voltage_now` | `0x22`/`0x23`, one 16-bit transfer so the two bytes come from the same conversion |
| `capacity` | the table, else `0x2A` |
| `temp` | `0x04` minus 40, in tenths of a degree |
| `charge_full_design`, `voltage_min_design`, `voltage_max_design` | the `monitored-battery` node, hidden without one |
| `manufacturer`, `model_name` | PiSugar, PiSugar 3 |

`pisugar3-mains` (`POWER_SUPPLY_TYPE_MAINS`): `online` from `0x02` bit 7, and
it supplies the battery, so a plug or unplug reaches the battery as an event.

The board is polled every ten seconds, and a uevent goes out when the status,
the online state or the percentage changes. Reads through sysfs return the
last poll; the board has no interrupt line to the Pi.

## Why not the vendor's module

PiSugar ship a kernel module of their own in
[`pisugar-power-manager-rs/pisugar-module/pisugar-3`](https://github.com/PiSugar/pisugar-power-manager-rs/tree/master/pisugar-module/pisugar-3).
It was read before a line of this one was written, and it is not a base for
anything upstream:

- It is a fork of `test_power.c`, the kernel's *fake* battery. The
  `charge_full` of 2000 mAh, the three-hour `time_to_empty`, the one-hour
  `time_to_full` and `health = Good` are constants inherited from that fake.
- It is not an I2C driver. It takes a bus number and an address as module
  parameters, creates the client itself and polls it from a kernel thread
  that sleeps in `TASK_UNINTERRUPTIBLE`; there is no device tree binding, no
  probe, no remove, no power management.
- `voltage_now` is reported in millivolts where the class is defined in
  microvolts, so every consumer shows a thousandth of the real voltage.
- Capacity is the board's `0x2A`, which the vendor's own userspace daemon
  ignores in favour of a voltage curve - a decision the people who built the
  hardware made, and the strongest hint in the whole register map.
- The two supplies register with no parent device and unregister in the
  wrong order on the error path.

Nothing for the PiSugar exists in mainline (`drivers/power/supply/`) or in
the Raspberry Pi kernel tree or its overlays, and the request for a driver
(PiSugar/PiSugar#16, 2020) went stale without a reply.

## Upstreaming

The driver is written to `checkpatch --strict` with a device tree binding in
the kernel's schema format, so that a mainline submission is a matter of
placing files:

- `drivers/power/supply/pisugar3_battery.c`
- `Documentation/devicetree/bindings/power/supply/pisugar,pisugar3.yaml`
- a `pisugar` entry in `vendor-prefixes.yaml`
- Kconfig `BATTERY_PISUGAR3`, Makefile, MAINTAINERS

The Raspberry Pi overlays go to the `raspberrypi/linux` tree's
`arch/arm/boot/dts/overlays/` separately, with their README entries. Both are
on the list; neither is sent yet.

## Why C and not Rust

Considered, and measured against the kernel this has to run on, 2026-09-06:

- **Raspberry Pi OS ships its kernels without `CONFIG_RUST`.** The 6.18
  `rpi-v8` config has `CONFIG_HAVE_RUST=y` and `CONFIG_RUST_IS_AVAILABLE=y`,
  which say the toolchain *could* build Rust, and no `CONFIG_RUST=y`, which
  says it did not. An out-of-tree Rust module needs the kernel's `core` and
  `kernel` crates built for that exact kernel, which the headers package does
  not carry. So a Rust driver would not load on the device this exists for
  without rebuilding its kernel.
- **The I2C client abstractions landed in 6.19**, one release after the Pi's
  kernel. On 6.18 there is no safe `i2c::Driver` to write against.
- **There is no `power_supply` abstraction in mainline Rust at all.** A Rust
  driver would have to add one - `power_supply_desc`, the property callback,
  `battery_info` and the OCV lookup - and get that reviewed first. That is a
  larger, separate contribution, and a fair one to make once a first user
  exists; a 400-line C driver is not the place to smuggle it in.

The C driver is the version that runs today and the version mainline will
take. If the abstractions arrive and Raspberry Pi OS turns Rust on, the
driver is small enough to rewrite in an afternoon, and this repository is
where that would happen.

## License

GPL-2.0-only. See `LICENSE`.
