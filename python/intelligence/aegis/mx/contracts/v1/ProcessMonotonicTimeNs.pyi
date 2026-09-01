from __future__ import annotations

import flatbuffers
import numpy as np

import typing

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class ProcessMonotonicTimeNs:
  @classmethod
  def SizeOf(cls) -> int: ...

  def Init(self, buf: bytes, pos: int) -> None: ...
  def Value(self) -> int: ...

def CreateProcessMonotonicTimeNs(builder: flatbuffers.Builder, value: int) -> uoffset: ...

