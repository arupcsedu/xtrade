from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class RiskCheckCode(IntEnum):
  NONE = cast(int, ...)
  TRADING_MODE_AUTHORIZATION = cast(int, ...)
  OPERATOR_SESSION_AUTHORIZATION = cast(int, ...)
  STRATEGY_AUTHORIZATION = cast(int, ...)
  SYMBOL_AUTHORIZATION = cast(int, ...)
  RESTRICTED_LIST = cast(int, ...)
  MARKET_STATE = cast(int, ...)
  HALT = cast(int, ...)
  CLOCK_HEALTH = cast(int, ...)
  FEED_BOOK_HEALTH = cast(int, ...)
  MAXIMUM_ORDER_QUANTITY = cast(int, ...)
  MAXIMUM_ORDER_NOTIONAL = cast(int, ...)
  PRICE_COLLAR = cast(int, ...)
  TICK_SIZE = cast(int, ...)
  DUPLICATE_INTENT = cast(int, ...)
  ORDER_RATE = cast(int, ...)
  CANCEL_RATE = cast(int, ...)
  SYMBOL_POSITION = cast(int, ...)
  GROSS_EXPOSURE = cast(int, ...)
  NET_EXPOSURE = cast(int, ...)
  SECTOR_CONCENTRATION = cast(int, ...)
  FACTOR_EXPOSURE = cast(int, ...)
  DAILY_LOSS = cast(int, ...)
  STRATEGY_LOSS = cast(int, ...)
  DRAWDOWN = cast(int, ...)
  CREDIT_CAPITAL = cast(int, ...)
  SHORT_SALE_LOCATE = cast(int, ...)
  SELF_TRADE_PREVENTION = cast(int, ...)
  VENUE_AUTHORIZATION = cast(int, ...)
  KILL_SWITCH = cast(int, ...)
  CONFIGURATION_FRESHNESS = cast(int, ...)

