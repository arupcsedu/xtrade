from __future__ import annotations

import flatbuffers
import numpy as np

import typing

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class ExchangeEventTimeNs:
  @classmethod
  def SizeOf(cls) -> int: ...

  def Init(self, buf: bytes, pos: int) -> None: ...
  def Value(self) -> int: ...

def CreateExchangeEventTimeNs(builder: flatbuffers.Builder, value: int) -> uoffset: ...

