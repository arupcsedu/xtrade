from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleDecisionReasonCode(IntEnum):
  UNKNOWN = cast(int, ...)
  COMBINED = cast(int, ...)
  INVALID_CONFIGURATION = cast(int, ...)
  INVALID_REQUEST = cast(int, ...)
  UNSAFE_MARKET_STATE = cast(int, ...)
  INVALID_INPUT_FEED = cast(int, ...)
  INVALID_TRANSACTION_COST = cast(int, ...)
  NO_ELIGIBLE_EXPERT = cast(int, ...)
  WEIGHT_CAP_INFEASIBLE = cast(int, ...)
  NO_POSITIVE_GATE_SCORE = cast(int, ...)
  INSUFFICIENT_ROBUST_EDGE = cast(int, ...)
  NUMERIC_ERROR = cast(int, ...)

