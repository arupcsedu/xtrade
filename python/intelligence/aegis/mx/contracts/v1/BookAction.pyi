from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class BookAction(IntEnum):
  UNKNOWN = cast(int, ...)
  ADD = cast(int, ...)
  CHANGE = cast(int, ...)
  DELETE = cast(int, ...)
  CLEAR_SIDE = cast(int, ...)
  CLEAR_BOOK = cast(int, ...)

