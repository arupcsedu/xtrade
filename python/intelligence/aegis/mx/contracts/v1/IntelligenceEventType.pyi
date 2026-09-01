from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class IntelligenceEventType(IntEnum):
  UNKNOWN = cast(int, ...)
  EARNINGS_RELEASE = cast(int, ...)
  GUIDANCE_UPDATE = cast(int, ...)
  MERGER_ACQUISITION = cast(int, ...)
  PRODUCT_RECALL = cast(int, ...)
  REGULATORY_ACTION = cast(int, ...)
  EXECUTIVE_CHANGE = cast(int, ...)
  FINANCING = cast(int, ...)
  BANKRUPTCY = cast(int, ...)
  LITIGATION = cast(int, ...)
  CYBERSECURITY_INCIDENT = cast(int, ...)
  ANALYST_ACTION = cast(int, ...)
  SUPPLY_CHAIN_DISRUPTION = cast(int, ...)
  TRADING_HALT = cast(int, ...)
  MACRO_RELEASE = cast(int, ...)
  RUMOR = cast(int, ...)
  CORRECTION = cast(int, ...)

