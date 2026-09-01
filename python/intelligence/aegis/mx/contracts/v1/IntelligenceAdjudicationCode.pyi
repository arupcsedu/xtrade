from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class IntelligenceAdjudicationCode(IntEnum):
  UNKNOWN = cast(int, ...)
  PRELIMINARY = cast(int, ...)
  CONFIRMED = cast(int, ...)
  REFINED = cast(int, ...)
  DISPUTED = cast(int, ...)
  CORRECTED = cast(int, ...)

