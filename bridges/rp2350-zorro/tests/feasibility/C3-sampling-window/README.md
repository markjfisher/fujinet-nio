# C3-sampling-window: Capture sampling window

**Status: planned, not implemented.** `./run.sh` reports this status and exits
without building, loading firmware or touching devices. No experiment pass is
claimed by the presence of this folder.

- Stimulus: Sweep data-change offset around /AS assertion.
- Expected observation: Characterize old/new transition and measurement uncertainty.
- Prerequisites: W0; configurable independent stimulus, DUT observation and adequate instrumentation.

[`experiment.json`](experiment.json) records this case's contract. Implement its
experiment-specific APIO source/configuration and failing-then-passing epio tests
here when this case is developed. Reference shared board/console support; do not
copy complete projects or manufacture placeholder firmware. Its starter must
then support the shared build/load/run/analyse controls and result format.

See the [experiment index](../README.md) and
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md). The existing
[generator check](../generator-check/README.md) validates test equipment only;
it does not satisfy this case.
