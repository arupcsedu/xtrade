from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class OmsInputKindCode(IntEnum):
  UNKNOWN = cast(int, ...)
  ACCEPT_INTENT = cast(int, ...)
  MARK_READY = cast(int, ...)
  DISPATCH = cast(int, ...)
  CANCEL_INTENT = cast(int, ...)
  REPLACE_INTENT = cast(int, ...)
  ACKNOWLEDGEMENT = cast(int, ...)
  REJECTION = cast(int, ...)
  FILL = cast(int, ...)
  CANCEL_ACKNOWLEDGEMENT = cast(int, ...)
  CANCEL_REJECTION = cast(int, ...)
  REPLACE_ACKNOWLEDGEMENT = cast(int, ...)
  REPLACE_REJECTION = cast(int, ...)
  EXPIRE = cast(int, ...)
  RECOVERY_BEGIN = cast(int, ...)
  RECOVERY_OBSERVATION = cast(int, ...)
  AUTHORITY_UPDATE = cast(int, ...)

