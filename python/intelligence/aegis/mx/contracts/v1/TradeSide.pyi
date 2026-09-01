from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class TradeSide(IntEnum):
  UNKNOWN = cast(int, ...)
  BUY = cast(int, ...)
  SELL = cast(int, ...)
  NOT_APPLICABLE = cast(int, ...)

