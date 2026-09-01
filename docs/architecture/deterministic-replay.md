# Deterministic replay

The replay subsystem consumes either a verified journal directory or an Aegis
synthetic `.smxcap` capture. It is offline-only and has no gateway or live-order
linkage.

## Modes

- `exact`: recorded model outputs and downstream records are delivered unchanged.
- `recompute`: each recorded model forecast is replaced through a registered
  recomputer using an explicitly selected model version. The run is labelled
  counterfactual. A missing model selection or recomputer fails closed.

Recompute dependencies are preflighted across the selected source before the
first event is delivered, preventing a late missing artifact from producing a
partial counterfactual run.

## Scheduling and controls

Source records are selected by inclusive time range and optional canonical
instrument ID before faults are applied. Original speed advances by the source
interval, accelerated speed divides intervals by an integer factor, maximum
speed does not wait, and single-step delivers one source record per call.
Pause and resume do not alter the cursor. All internal time is supplied by a
deterministic simulated clock; a real pacer is only a CLI boundary for offline
original-speed runs.

Loading is bounded by both record count and total payload bytes. Defaults are
one million selected records and 1 GiB; hard ceilings are ten million records
and 16 GiB. Exceeding either limit fails before event delivery starts.

Faults are stable rules over source ordinals: latency, loss, duplication,
adjacent reordering, integer clock drift, feed outage, process crash, halt,
reopening, and opaque news or macro event injection. The seed participates in
deterministic fault selection. Injection cannot invoke process commands, change
configuration, or transmit orders.

## Reproducibility evidence

Each manifest contains the source dataset SHA-256, replay configuration hash,
selected model versions, deterministic seed, source/mode, and build/environment
metadata. The summary has a hash chain for every reproducible output category
and a final hash over the ordered run. Comparing final hashes is meaningful only
when dataset and configuration hashes match.

When an application configuration hash is not supplied, the engine records a
nonzero SHA-256 of the complete replay selection/fault configuration. The
manifest self-hash is calculated with its own field zeroed, then stored in the
emitted document.

Synthetic packet replay decodes the documented mock protocol and reconstructs
the expected final synthetic book. Canonical journal replay validates every
audit envelope and reproduces recorded feature, forecast, ensemble, risk, OMS,
fill, position, and P&L artifacts. Integrations can register production
component adapters as replay targets; replay itself does not duplicate their
business logic.
