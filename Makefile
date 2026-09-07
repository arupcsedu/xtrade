SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

.PHONY: help bootstrap format format-check lint test test-sanitizers test-fuzz benchmark benchmark-platform benchmark-platform-smoke benchmark-regression paper-integration package docs-check schemas-check schemas-generate dependency-scan security-test chaos-fast chaos-nightly edge-validate edge-package regional-validate reproducibility-check fast full

help:
	@tools/run.sh help

bootstrap:
	@tools/run.sh bootstrap

format:
	@tools/run.sh format

format-check:
	@tools/run.sh format-check

lint:
	@tools/run.sh lint

test:
	@tools/run.sh test

test-sanitizers:
	@tools/run.sh test-sanitizers

test-fuzz:
	@tools/run.sh test-fuzz

benchmark:
	@tools/run.sh benchmark

benchmark-platform:
	@tools/run.sh benchmark-platform

benchmark-platform-smoke:
	@tools/run.sh benchmark-platform-smoke

benchmark-regression:
	@tools/run.sh benchmark-regression

paper-integration:
	@tools/run.sh paper-integration

package:
	@tools/run.sh package

docs-check:
	@tools/run.sh docs-check

schemas-check:
	@tools/run.sh schemas-check

schemas-generate:
	@tools/run.sh schemas-generate

dependency-scan:
	@tools/run.sh dependency-scan

security-test:
	@tools/run.sh security-test

chaos-fast:
	@tools/run.sh chaos-fast

chaos-nightly:
	@tools/run.sh chaos-nightly

edge-validate:
	@tools/run.sh edge-validate

edge-package:
	@tools/run.sh edge-package

regional-validate:
	@tools/run.sh regional-validate

reproducibility-check:
	@tools/run.sh reproducibility-check

fast:
	@tools/run.sh fast

full:
	@tools/run.sh full
