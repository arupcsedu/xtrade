from __future__ import annotations

import flatbuffers
import numpy as np

import typing
from enum import IntEnum
from typing import cast

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class ContractPayload(IntEnum):
  NONE = cast(int, ...)
  InstrumentReferenceData = cast(int, ...)
  MarketEvent = cast(int, ...)
  BookUpdate = cast(int, ...)
  TradeEvent = cast(int, ...)
  QuoteEvent = cast(int, ...)
  AuctionImbalance = cast(int, ...)
  TradingStatus = cast(int, ...)
  FeatureSnapshotMetadata = cast(int, ...)
  ModelForecast = cast(int, ...)
  EnsembleForecast = cast(int, ...)
  OrderIntent = cast(int, ...)
  RiskDecision = cast(int, ...)
  OrderEvent = cast(int, ...)
  FillEvent = cast(int, ...)
  PositionSnapshot = cast(int, ...)
  EventIntelligenceRecord = cast(int, ...)
  DataQualityState = cast(int, ...)
  ClockQualityState = cast(int, ...)
  KillSwitchEvent = cast(int, ...)

