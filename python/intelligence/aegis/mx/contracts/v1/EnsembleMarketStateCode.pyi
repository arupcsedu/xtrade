from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleMarketStateCode(IntEnum):
  UNKNOWN = cast(int, ...)
  STARTUP = cast(int, ...)
  NORMAL = cast(int, ...)
  SCHEDULED_EVENT = cast(int, ...)
  BREAKING_NEWS = cast(int, ...)
  EVENT_PRICE_DISCOVERY = cast(int, ...)
  VOLATILITY_SPIKE = cast(int, ...)
  DATA_DEGRADED = cast(int, ...)
  HALTED = cast(int, ...)
  REOPENING = cast(int, ...)
  RECOVERY = cast(int, ...)
  SHUTDOWN = cast(int, ...)

