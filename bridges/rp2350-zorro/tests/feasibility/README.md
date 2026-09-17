# Story 2.2 experiments

Start with **[generator-check](generator-check/README.md)**. It is the independent
RP2040 equipment check we already captured, now packaged for you to build, load,
run and analyse. It is **not C1 completion**: C1 needs the RP2350 to capture and
report the stimulus. The RP2350 observer has not been implemented.

| Folder | Purpose | Status |
| --- | --- | --- |
| [generator-check](generator-check/README.md) | Independent 0–15 W0 waveform | Implemented; interactive runner plus separate stages |
| [C0-idle](C0-idle/README.md) | No capture without assertion | Planned |
| [C1-patterns](C1-patterns/README.md) | Exact captured patterns and counts | Planned |
| [C2-held-active](C2-held-active/README.md) | One capture while /AS stays asserted | Planned |
| [C3-sampling-window](C3-sampling-window/README.md) | Data sampling transition | Planned |
| [C4-repetition](C4-repetition/README.md) | Pulse/gap limits | Planned |
| [C5-width-control](C5-width-control/README.md) | Wider data, direction and selection | Planned |
| [C6-pressure](C6-pressure/README.md) | FIFO pressure and explicit loss | Planned |
| [C7-reads](C7-reads/README.md) | Read response timing | Planned |
| [C8-turnaround](C8-turnaround/README.md) | Direction changes/output release | Planned |
| [C9-recovery](C9-recovery/README.md) | Reset, abort and recovery | Planned |
| [C10-real-bus](C10-real-bus/README.md) | Buffered actual-host validation | Planned; later hardware required |

C0 is retained: idle capture suppression is a useful baseline. Planned starters
fail explicitly without touching hardware. As each case is implemented, its
folder must contain its own source/configuration and expectations, point to
shared support code, and expose the same stage controls. This avoids separate
ad hoc projects and duplicated firmware support.

## Your controls

Run `generator-check/run.sh --help` from any working directory. The default
interactive flow and `all` build/check software, guide BOOTSEL identification and
RAM loading, and wait for your explicit instruction before generating signals.
Separate `doctor`, `build`, `load`, `run` and `analyse` stages let you inspect or
repeat individual steps. `--dry-run` previews operations without touching devices.
Builds never generate signals. No experiment stage runs sudo or changes system
permissions. Ctrl-C cancels the host workflow; the runner attempts stop and cleans
up acquisition, while firmware independently limits each burst.

Results belong in fresh directories under the bridge's ignored `build/` tree
(or the explicit `--output` path): source/artifact identity, console and tool logs,
raw `.sr` capture, expected/observed measurements and a machine-readable verdict.
Failed runs remain evidence; a busy analyzer or missing capture cannot pass.
PulseView can open saved captures once sigrok-cli releases the device.
Build and loader command logs are retained under `build/feasibility/stage-logs/`.

## One-time Linux access setup

First run `generator-check/run.sh doctor`. Build prerequisites are the bridge's
[existing toolchain setup](../../README.md); capture additionally uses
`sigrok-cli`, the installed fx2lafw firmware and a USB-enabled pinned picotool.
Close PulseView's live device while sigrok-cli owns the analyzer.

If normal-user access to RP2040 BOOTSEL or its serial console is missing, review
[69-nio-feasibility.rules](69-nio-feasibility.rules). For a systemd desktop session,
you may install it **once**, explicitly, from this directory:

```sh
sudo install -m 0644 69-nio-feasibility.rules /etc/udev/rules.d/69-nio-feasibility.rules
sudo udevadm control --reload-rules
```

Reconnect the RP2040 after installation. The rule grants the active local desktop
user access to RP2040 BOOTSEL (`2e8a:0003`) and SDK USB/serial (`2e8a:000a`). It
covers those device classes, not just this board; it does not select a target or
authorize a load. The runner separately verifies the intended flash identity and
physical USB path. It does not grant RP2350 (`2e8a:0009`) access. Your analyzer's
packaged sigrok rules remain responsible for analyzer access. The `69-` ordering
is intentional: the tag must exist before systemd's `73-seat-late.rules` applies it.

For SSH/headless hosts without an active local seat, this rule may not grant
access: arrange an appropriate device-access group with the machine administrator
before running hardware stages. The runner reports missing access and stops;
it does not fall back to arbitrary devices or a sudo retry. No device-specific
`setfacl` sequence from the original session is part of the repeatable procedure.

The [Story 2.2 plan](../../docs/story-2-2-experiment-plan.md) remains authoritative
for test-first PIO behavior, fixture changes, evidence and later real-bus gates.
