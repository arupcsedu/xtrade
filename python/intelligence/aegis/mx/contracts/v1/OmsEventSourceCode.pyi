from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class OmsEventSourceCode(IntEnum):
  UNKNOWN = cast(int, ...)
  INTERNAL = cast(int, ...)
  GATEWAY = cast(int, ...)
  DROP_COPY = cast(int, ...)
  RECOVERY = cast(int, ...)

