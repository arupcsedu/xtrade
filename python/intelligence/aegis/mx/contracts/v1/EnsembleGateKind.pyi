from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class EnsembleGateKind(IntEnum):
  UNKNOWN = cast(int, ...)
  RULE_BASED = cast(int, ...)
  LINEAR = cast(int, ...)
  QUANTIZED_LEARNED = cast(int, ...)

