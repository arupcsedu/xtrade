from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleDominantExpertReasonCode(IntEnum):
  UNKNOWN = cast(int, ...)
  HIGHEST_WEIGHT = cast(int, ...)
  CANONICAL_TIE_BREAK = cast(int, ...)
  ONLY_ELIGIBLE_EXPERT = cast(int, ...)
  NO_ELIGIBLE_EXPERT = cast(int, ...)

