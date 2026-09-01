from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleEligibilityCode(IntEnum):
  UNKNOWN = cast(int, ...)
  ELIGIBLE = cast(int, ...)
  MISSING_PROVENANCE = cast(int, ...)
  EXPIRED = cast(int, ...)
  MODEL_DISABLED = cast(int, ...)
  INVALID_INPUT_FEED = cast(int, ...)
  INCOMPATIBLE_STATE = cast(int, ...)
  EXCESSIVE_OOD = cast(int, ...)
  CALIBRATION_FAILED = cast(int, ...)
  DATA_QUALITY_FAILED = cast(int, ...)
  INVALID_FORECAST = cast(int, ...)
  SCOPE_MISMATCH = cast(int, ...)
  CONTROL_GENERATION_MISMATCH = cast(int, ...)
  DUPLICATE_EXPERT = cast(int, ...)

