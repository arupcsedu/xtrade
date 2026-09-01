from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class KillSwitchScope(IntEnum):
  UNKNOWN = cast(int, ...)
  GLOBAL = cast(int, ...)
  VENUE = cast(int, ...)
  STRATEGY = cast(int, ...)
  SYMBOL = cast(int, ...)
  ACCOUNT = cast(int, ...)

