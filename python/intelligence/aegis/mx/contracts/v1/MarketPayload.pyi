from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class MarketPayload(IntEnum):
  NONE = cast(int, ...)
  BookUpdate = cast(int, ...)
  TradeEvent = cast(int, ...)
  QuoteEvent = cast(int, ...)
  AuctionImbalance = cast(int, ...)
  TradingStatus = cast(int, ...)

