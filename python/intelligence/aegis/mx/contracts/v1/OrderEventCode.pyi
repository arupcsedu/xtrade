from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class OrderEventCode(IntEnum):
  UNKNOWN = cast(int, ...)
  ACCEPTED_BY_OMS = cast(int, ...)
  SUBMITTED_TO_GATEWAY = cast(int, ...)
  ACKNOWLEDGED = cast(int, ...)
  PARTIALLY_FILLED = cast(int, ...)
  FILLED = cast(int, ...)
  CANCEL_PENDING = cast(int, ...)
  CANCELLED = cast(int, ...)
  REJECTED = cast(int, ...)

