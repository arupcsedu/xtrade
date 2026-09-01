from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class TradingStatusCode(IntEnum):
  UNKNOWN = cast(int, ...)
  PRE_OPEN = cast(int, ...)
  OPEN = cast(int, ...)
  HALTED = cast(int, ...)
  AUCTION = cast(int, ...)
  CLOSED = cast(int, ...)

