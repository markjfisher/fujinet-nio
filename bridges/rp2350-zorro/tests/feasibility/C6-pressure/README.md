# C6-pressure: FIFO pressure and backpressure

**Status: planned, not implemented.** `./run.sh` reports this status and exits
without building, loading firmware or touching devices. No experiment pass is
claimed by the presence of this folder.

- Stimulus: Pause or slow DUT drain then resume.
- Expected observation: Account for all samples, stalls, losses and timeouts.
- Prerequisites: W1 capture/drain controls; separate baseline and handshake variants.

[`experiment.json`](experiment.json) records this case's contract. Implement its
experiment-specific APIO source/configuration and failing-then-passing epio tests
here when this case is developed. Reference shared board/console support; do not
copy complete projects or manufacture placeholder firmware. Its starter must
then support the shared build/load/run/analyse controls and result format.

See the [experiment index](../README.md) and
[Story 2.2 plan](../../../docs/story-2-2-experiment-plan.md). The existing
[generator check](../generator-check/README.md) validates test equipment only;
it does not satisfy this case.
