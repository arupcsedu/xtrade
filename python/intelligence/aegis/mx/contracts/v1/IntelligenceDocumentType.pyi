from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class IntelligenceDocumentType(IntEnum):
  UNKNOWN = cast(int, ...)
  NEWS_ARTICLE = cast(int, ...)
  FILING = cast(int, ...)
  PRESS_RELEASE = cast(int, ...)
  REGULATORY_NOTICE = cast(int, ...)
  MACRO_RELEASE = cast(int, ...)
  CORRECTION = cast(int, ...)

