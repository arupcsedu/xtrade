from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class PortfolioHealthCode(IntEnum):
  UNKNOWN = cast(int, ...)
  STARTING = cast(int, ...)
  HEALTHY = cast(int, ...)
  RECONCILING = cast(int, ...)
  UNSAFE = cast(int, ...)
  STOPPED = cast(int, ...)

