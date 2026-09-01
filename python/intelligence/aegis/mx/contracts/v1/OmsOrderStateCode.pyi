from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class OmsOrderStateCode(IntEnum):
  UNKNOWN = cast(int, ...)
  CREATED = cast(int, ...)
  READY = cast(int, ...)
  PENDING_ACK = cast(int, ...)
  WORKING = cast(int, ...)
  PARTIALLY_FILLED = cast(int, ...)
  PENDING_CANCEL = cast(int, ...)
  PENDING_REPLACE = cast(int, ...)
  FILLED = cast(int, ...)
  CANCELED = cast(int, ...)
  REJECTED = cast(int, ...)
  EXPIRED = cast(int, ...)
  UNKNOWN_RECOVERY = cast(int, ...)

