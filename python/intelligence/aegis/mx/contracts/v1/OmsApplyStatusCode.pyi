from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class OmsApplyStatusCode(IntEnum):
  UNKNOWN = cast(int, ...)
  APPLIED = cast(int, ...)
  DUPLICATE = cast(int, ...)
  RECONCILED = cast(int, ...)
  OUT_OF_ORDER_APPLIED = cast(int, ...)
  FORBIDDEN_TRANSITION = cast(int, ...)
  INVALID = cast(int, ...)
  NOT_FOUND = cast(int, ...)
  CAPACITY_EXHAUSTED = cast(int, ...)
  JOURNAL_UNAVAILABLE = cast(int, ...)
  FENCED = cast(int, ...)
  INHIBITED = cast(int, ...)
  BUSY = cast(int, ...)
  STOPPED = cast(int, ...)

