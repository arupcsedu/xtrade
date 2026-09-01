from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class KillSwitchReasonCode(IntEnum):
  UNKNOWN = cast(int, ...)
  OPERATOR = cast(int, ...)
  RISK_SERVICE = cast(int, ...)
  CLOCK_QUALITY = cast(int, ...)
  MARKET_DATA_QUALITY = cast(int, ...)
  SPLIT_BRAIN = cast(int, ...)
  TRADING_HALT = cast(int, ...)
  INTERNAL_FAULT = cast(int, ...)

