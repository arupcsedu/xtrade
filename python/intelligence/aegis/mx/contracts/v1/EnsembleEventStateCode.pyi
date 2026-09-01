from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleEventStateCode(IntEnum):
  UNKNOWN = cast(int, ...)
  NONE = cast(int, ...)
  SCHEDULED = cast(int, ...)
  BREAKING_NEWS = cast(int, ...)
  PRICE_DISCOVERY = cast(int, ...)
  VOLATILITY_SHOCK = cast(int, ...)

