from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class PortfolioInvariantCode(IntEnum):
  UNKNOWN = cast(int, ...)
  NONE = cast(int, ...)
  INVALID_CONFIGURATION = cast(int, ...)
  CONFLICTING_FILL = cast(int, ...)
  ORPHAN_FILL = cast(int, ...)
  ACCOUNTING_MISMATCH = cast(int, ...)
  ARITHMETIC_OVERFLOW = cast(int, ...)
  STALE_EVENT = cast(int, ...)
  UNSUPPORTED_CORPORATE_ACTION = cast(int, ...)
  JOURNAL_EXHAUSTED = cast(int, ...)

