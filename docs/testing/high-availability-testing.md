# High-availability verification

The HA suite uses only injected monotonic timestamps and fixed identifiers; it
does not sleep, open a network connection, require a secret, or enable live
trading. No random source is used, so no test seed applies.

## Focused commands

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/cmake --preset dev
/scratch/djy8hg/env/aegis_mx_contracts/bin/cmake --build --preset dev \
  --target aegis_high_availability_tests -j 4
ctest --test-dir build/dev --output-on-failure \
  -R aegis_high_availability_tests
```

The repository `make test-sanitizers` target remains the canonical ASan, UBSan,
and TSan gate. The release benchmark cases are
`BM_HaFailClosedAuthorityCheck` and `BM_RecoveryAppendApply`.

## Scenario coverage

| Requirement/failure | Test evidence |
| --- | --- |
| Active leader and hot standby | standby synchronization and atomic snapshot test |
| Leader crash | caught-up standby promotion integration test |
| Standby crash | active-degraded test |
| Network partition/lease loss | lease-expiry fencing test |
| Split brain | live peer leadership test and stale old-owner emission test |
| Delayed heartbeat | non-revivable missed-heartbeat test |
| Stale shared memory | dependency failure table test |
| Gateway disconnect | reconciliation-required test |
| Risk restart | changed risk-epoch test |
| Journal failure | terminal unsafe test |
| Partial colocation outage | explicit outage fencing test |
| Replication lag | promotion refusal integration test |
| Duplicate acknowledged order | replicated emission/ack failover test |
| Recovery overload | bounded queue exhaustion test |
| Malformed grant | deterministic mutation table test |
| Remote region | risk-reduction-only non-emission test |

The reference tests prove local state-machine and transport contracts. A
production certification must additionally inject real process termination,
network partitions, witness loss, device failure, and gateway-session fencing on
the target colocation topology. Licensed venue recovery behavior cannot be
proved by these synthetic tests.
