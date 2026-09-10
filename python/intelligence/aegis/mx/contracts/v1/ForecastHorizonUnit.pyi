from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class ForecastHorizonUnit(IntEnum):
  UNKNOWN = cast(int, ...)
  ELAPSED_NANOSECONDS = cast(int, ...)
  TRADING_MINUTES = cast(int, ...)
  TRADING_SESSIONS = cast(int, ...)

