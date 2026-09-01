from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class DataQualityCode(IntEnum):
  UNKNOWN = cast(int, ...)
  VALID = cast(int, ...)
  DEGRADED = cast(int, ...)
  STALE = cast(int, ...)
  INVALID = cast(int, ...)

