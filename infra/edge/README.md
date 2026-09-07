# Colocated edge deployment assets

This directory contains non-live systemd contracts, immutable environment profiles, host tuning examples, tmpfiles, and log rotation. Nothing here installs itself, changes the host, enables a unit, or contains credentials.

Use `edge-deployment validate-assets`, `render`, `validate-host`, and `package-rollback`. Review [the architecture](../../docs/architecture/colocated-edge-deployment.md) and [operator checklist](../../docs/operations/colocated-edge-deployment-checklist.md) before host changes.
