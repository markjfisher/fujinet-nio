# C9-recovery: Reset and recovery

**Status: planned, not implemented.** `./run.sh` reports this status and exits
without building, loading firmware or touching devices. No experiment pass is
claimed by the presence of this folder.

- Stimulus: Abort, reset/reboot either board, disconnect console.
- Expected observation: Defined idle/released outputs and no stale-run data; rearm correctly.
- Prerequisites: Both firmware roles with bounded cleanup; explicit physical fault procedure.

[`experiment.json`](experiment.json) records this case's contract. Implement its
experiment-specific APIO source/configuration and failing-then-passing epio tests
here when this case is developed. Reference shared board/console support; do not
copy complete projects or manufacture placeholder firmware. Its starter must
then support the shared build/load/run/analyse controls and result format.

See the [experiment index](../README.md) and
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md). The existing
[generator check](../generator-check/README.md) validates test equipment only;
it does not satisfy this case.
