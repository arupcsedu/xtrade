SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

.PHONY: help bootstrap format format-check lint test test-sanitizers test-fuzz benchmark package docs-check schemas-check schemas-generate dependency-scan fast full

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

fast:
	@tools/run.sh fast

full:
	@tools/run.sh full
