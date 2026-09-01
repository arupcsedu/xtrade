from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class ForecastUnit(IntEnum):
  UNKNOWN = cast(int, ...)
  RETURN_TICKS = cast(int, ...)
  FAIR_VALUE_TICKS = cast(int, ...)
  PROBABILITY_PPM = cast(int, ...)
  QUANTITY_UNITS = cast(int, ...)
  RETURN_PPM = cast(int, ...)
  VOLATILITY_PPM = cast(int, ...)
  VOLUME_UNITS = cast(int, ...)
  SPREAD_TICKS = cast(int, ...)
  FACTOR_PPM = cast(int, ...)

