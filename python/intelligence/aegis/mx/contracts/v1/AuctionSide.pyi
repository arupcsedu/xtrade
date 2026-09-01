from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class AuctionSide(IntEnum):
  UNKNOWN = cast(int, ...)
  BUY_IMBALANCE = cast(int, ...)
  SELL_IMBALANCE = cast(int, ...)
  PAIRED = cast(int, ...)

