from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class RiskReasonCode(IntEnum):
  UNKNOWN = cast(int, ...)
  WITHIN_LIMITS = cast(int, ...)
  KILL_SWITCH_ENGAGED = cast(int, ...)
  STALE_MARKET_DATA = cast(int, ...)
  INVALID_BOOK = cast(int, ...)
  CLOCK_UNSYNCHRONIZED = cast(int, ...)
  LIMIT_EXCEEDED = cast(int, ...)
  INVALID_INTENT = cast(int, ...)
  CONFIGURATION_MISMATCH = cast(int, ...)
  EXPIRED_INTENT = cast(int, ...)
  TRADING_MODE_UNAUTHORIZED = cast(int, ...)
  OPERATOR_SESSION_UNAUTHORIZED = cast(int, ...)
  STRATEGY_UNAUTHORIZED = cast(int, ...)
  SYMBOL_UNAUTHORIZED = cast(int, ...)
  RESTRICTED_INSTRUMENT = cast(int, ...)
  MARKET_STATE_UNSAFE = cast(int, ...)
  TRADING_HALTED = cast(int, ...)
  FEED_BOOK_UNHEALTHY = cast(int, ...)
  ORDER_QUANTITY_EXCEEDED = cast(int, ...)
  ORDER_NOTIONAL_EXCEEDED = cast(int, ...)
  PRICE_COLLAR_EXCEEDED = cast(int, ...)
  INVALID_TICK_SIZE = cast(int, ...)
  DUPLICATE_INTENT = cast(int, ...)
  ORDER_RATE_EXCEEDED = cast(int, ...)
  CANCEL_RATE_EXCEEDED = cast(int, ...)
  SYMBOL_POSITION_EXCEEDED = cast(int, ...)
  GROSS_EXPOSURE_EXCEEDED = cast(int, ...)
  NET_EXPOSURE_EXCEEDED = cast(int, ...)
  SECTOR_CONCENTRATION_EXCEEDED = cast(int, ...)
  FACTOR_EXPOSURE_EXCEEDED = cast(int, ...)
  DAILY_LOSS_EXCEEDED = cast(int, ...)
  STRATEGY_LOSS_EXCEEDED = cast(int, ...)
  DRAWDOWN_EXCEEDED = cast(int, ...)
  CREDIT_CAPITAL_EXCEEDED = cast(int, ...)
  SHORT_SALE_LOCATE_DENIED = cast(int, ...)
  SELF_TRADE_PREVENTION_DENIED = cast(int, ...)
  VENUE_UNAUTHORIZED = cast(int, ...)
  CONFIGURATION_STALE = cast(int, ...)
  STALE_POSITION = cast(int, ...)
  RISK_STATE_UNAVAILABLE = cast(int, ...)
  CONFIGURATION_ROLLBACK = cast(int, ...)
  SPLIT_BRAIN_EPOCH = cast(int, ...)
  ARITHMETIC_OVERFLOW = cast(int, ...)
  JOURNAL_UNAVAILABLE = cast(int, ...)
  ENGINE_BUSY = cast(int, ...)

