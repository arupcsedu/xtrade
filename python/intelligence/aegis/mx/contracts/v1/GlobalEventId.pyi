from __future__ import annotations

import flatbuffers
import numpy as np

import typing

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class GlobalEventId:
  @classmethod
  def SizeOf(cls) -> int: ...

  def Init(self, buf: bytes, pos: int) -> None: ...
  def High(self) -> int: ...
  def Low(self) -> int: ...

def CreateGlobalEventId(builder: flatbuffers.Builder, high: int, low: int) -> uoffset: ...

