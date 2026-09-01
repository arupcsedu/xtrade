from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class ForecastTarget(IntEnum):
  UNKNOWN = cast(int, ...)
  RETURN = cast(int, ...)
  REALIZED_VOLATILITY = cast(int, ...)
  VOLUME = cast(int, ...)
  SPREAD = cast(int, ...)
  MARKET_FACTOR = cast(int, ...)
  SECTOR_FACTOR = cast(int, ...)

