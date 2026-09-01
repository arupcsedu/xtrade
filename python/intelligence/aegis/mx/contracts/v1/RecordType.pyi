from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class RecordType(IntEnum):
  UNKNOWN = cast(int, ...)
  INSTRUMENT_REFERENCE_DATA = cast(int, ...)
  MARKET_EVENT = cast(int, ...)
  BOOK_UPDATE = cast(int, ...)
  TRADE_EVENT = cast(int, ...)
  QUOTE_EVENT = cast(int, ...)
  AUCTION_IMBALANCE = cast(int, ...)
  TRADING_STATUS = cast(int, ...)
  FEATURE_SNAPSHOT_METADATA = cast(int, ...)
  MODEL_FORECAST = cast(int, ...)
  ENSEMBLE_FORECAST = cast(int, ...)
  ORDER_INTENT = cast(int, ...)
  RISK_DECISION = cast(int, ...)
  ORDER_EVENT = cast(int, ...)
  FILL_EVENT = cast(int, ...)
  POSITION_SNAPSHOT = cast(int, ...)
  EVENT_INTELLIGENCE_RECORD = cast(int, ...)
  DATA_QUALITY_STATE = cast(int, ...)
  CLOCK_QUALITY_STATE = cast(int, ...)
  KILL_SWITCH_EVENT = cast(int, ...)

