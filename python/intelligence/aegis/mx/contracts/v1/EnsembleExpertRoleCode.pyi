from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleExpertRoleCode(IntEnum):
  UNKNOWN = cast(int, ...)
  GENERIC = cast(int, ...)
  MICROSTRUCTURE = cast(int, ...)
  TIMESERIES = cast(int, ...)
  EVENT = cast(int, ...)
  OPTIONS = cast(int, ...)
  MACRO = cast(int, ...)
  AUCTION = cast(int, ...)

