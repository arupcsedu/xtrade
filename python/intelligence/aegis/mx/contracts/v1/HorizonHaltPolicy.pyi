from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class HorizonHaltPolicy(IntEnum):
  UNKNOWN = cast(int, ...)
  NOT_APPLICABLE = cast(int, ...)
  REJECT = cast(int, ...)
  PAUSE = cast(int, ...)
  COUNT_SCHEDULED = cast(int, ...)

