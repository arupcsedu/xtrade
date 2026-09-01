from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class TradingModeCode(IntEnum):
  UNKNOWN = cast(int, ...)
  SIMULATION = cast(int, ...)
  PAPER = cast(int, ...)
  LIVE = cast(int, ...)

