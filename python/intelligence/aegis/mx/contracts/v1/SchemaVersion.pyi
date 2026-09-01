from __future__ import annotations

import flatbuffers
import numpy as np

import typing

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class SchemaVersion:
  @classmethod
  def SizeOf(cls) -> int: ...

  def Init(self, buf: bytes, pos: int) -> None: ...
  def Major(self) -> int: ...
  def Minor(self) -> int: ...
  def Patch(self) -> int: ...

def CreateSchemaVersion(builder: flatbuffers.Builder, major: int, minor: int, patch: int) -> uoffset: ...

