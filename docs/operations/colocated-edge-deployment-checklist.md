# Colocated edge deployment checklist

## Hardware assumptions

- Server-class x86-64 or AArch64 host with invariant monotonic counter and stable firmware.
- One NUMA-local CPU socket for the reference edge allocation and enough CPUs to retain housekeeping capacity.
- ECC memory sized for preallocated books, queues, model state, journal buffers, and huge pages.
- Dedicated enterprise NVMe with power-loss protection for the local journal.
- NIC with hardware RX timestamps, PHC, at least four verified receive queues, supported firmware, and measured line-rate behavior.
- Redundant power, an out-of-band management path, and a PTP source whose identity and holdover behavior are known.
- systemd 252 or newer; the checked-in hardening and NUMA directives are not qualified on older supervisors.

## BIOS and firmware review

- Pin an approved BIOS, CPU microcode, NIC firmware, and NVMe firmware inventory.
- Select the measured performance power profile; disable deep C-states and opportunistic frequency behavior only when benchmarks prove this is necessary.
- Disable NUMA interleaving and confirm memory channels are populated symmetrically.
- Record SMT policy. If SMT is enabled, do not allocate sibling threads to unrelated latency-sensitive roles.
- Disable unneeded devices and boot features only through the normal security/change process.
- Never weaken Secure Boot, IOMMU, mitigations, or access controls merely to improve a benchmark.

## Pre-install

1. Record host facts, topology (`lscpu -e`, `numactl --hardware`), kernel, firmware, NIC driver, queue count, timestamp capabilities, and NVMe health.
2. Derive a site-owned profile from the staging or production-disabled base and replace the CPU/NIC placeholder with approved values. Keep that resolved profile outside the repository.
3. Review the kernel command-line example against actual housekeeping CPUs and IRQ ownership.
4. Stage the sysctl, tmpfiles, logrotate, and systemd files through configuration management; do not run repository scripts as root.
5. Install and review the supplied `sysusers.d` definition for the unprivileged `aegis-edge` account; restrict release/configuration ownership to administrators.
6. Grant the clock guard read-only access to the exact PHC device through a site-managed group or device ACL; do not grant broad time-setting capabilities.
7. Mount journal storage by stable UUID and verify the filesystem, free-space alarms, write cache, and power-loss protection.
8. Install a release under its artifact hash. Confirm `/opt/aegis-mx/current` is not changed by unpacking.
9. Verify package and SBOM hashes and the signed runtime configuration. No credential may appear in an environment file or command line.

## Readiness and activation

1. Run `edge-deployment validate-assets` and validate the selected profile.
2. Capture host facts and run `edge-deployment validate-host --profile-path resolved-profile.json --facts host-facts.json`. A placeholder NIC must fail.
3. Run `edge-deployment render --profile-path resolved-profile.json --output empty-staging-directory`; inspect every drop-in and queue assignment before copying the active profile and facts to `/etc/aegis-mx/deployment/`.
4. Run `systemd-analyze verify` on the staged units and inspect `systemd-analyze security` results.
5. Reboot after approved huge-page, isolation, or sysctl changes. Confirm no unexpected task or IRQ uses isolated CPUs.
6. Verify PTP source identity, fresh synchronization, PHC mapping, offset, and hardware receive timestamps.
7. Confirm the journal recovery scan, free space, segment rotation, permissions, and fsync latency.
8. Confirm the startup initializer created the inhibit record. Only the approved control/kill-switch workflow may clear it for explicitly authorized non-live commissioning; readiness must not clear it.
9. Start only in simulation or paper mode. Confirm every log and metric carries that mode.
10. Verify build/version, health, readiness, configuration hash, metrics, structured logs, watchdog, and graceful shutdown for each installed daemon.
11. Run the performance validation suite and record the artifact/profile/host hashes.
12. Obtain the required operator approvals before selecting `aegis-edge.target`; never enable it automatically from a package.

## Shutdown

Stop new intents, inhibit transmission, wait for the paper gateway to quiesce, drain the edge core, flush/rotate the journal, then stop clock monitoring. Verify final position/order snapshots and journal hashes. Systemd dependency order implements this reversal, but the operator must confirm reconciliation before maintenance.

## Rollback trigger

Rollback for failed readiness, dependency mismatch, latency regression, clock instability, journal errors, unexpected CPU/IRQ placement, missing health records, or any safety-control regression. Follow [Colocated edge rollback](colocated-edge-rollback.md); rollback never implies reactivation.
