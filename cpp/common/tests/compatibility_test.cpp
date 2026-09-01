#include "aegis/mx/contracts/v1/compatibility_v1_1_generated.h"
#include "aegis/mx/contracts/v1/compatibility_v1_2_generated.h"
#include "aegis/mx/contracts/v1/compatibility_v1_3_generated.h"
#include "aegis/mx/contracts/v1/compatibility_v1_4_generated.h"
#include "aegis/mx/contracts/v1/compatibility_v1_5_generated.h"
#include "aegis/mx/contracts/v1/compatibility_v1_6_generated.h"
#include "aegis/mx/contracts/v1/compatibility_v1_generated.h"

#include <gtest/gtest.h>

#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/verifier.h>

namespace {

namespace v1 = aegis::mx::contracts::compat::v1;
namespace v1_1 = aegis::mx::contracts::compat::v1_1;
namespace v1_2 = aegis::mx::contracts::compat::v1_2;
namespace v1_3 = aegis::mx::contracts::compat::v1_3;
namespace v1_4 = aegis::mx::contracts::compat::v1_4;
namespace v1_5 = aegis::mx::contracts::compat::v1_5;
namespace v1_6 = aegis::mx::contracts::compat::v1_6;

TEST(CompatibilityTest, OlderReaderIgnoresAdditiveField) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1_1::CreateCompatibilityProbe(builder, 41U, 99U);
  v1_1::FinishCompatibilityProbeBuffer(builder, probe);

  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1::VerifyCompatibilityProbeBuffer(verifier));
  EXPECT_EQ(v1::GetCompatibilityProbe(builder.GetBufferPointer())->stable_value(), 41U);
}

TEST(CompatibilityTest, NewerReaderDefaultsMissingAdditiveField) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1::CreateCompatibilityProbe(builder, 41U);
  v1::FinishCompatibilityProbeBuffer(builder, probe);

  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_1::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_1::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->additive_value(), 0U);
}

TEST(CompatibilityTest, V12ReaderIgnoresV13Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1_3::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U);
  v1_3::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_2::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_2::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->second_additive_value(), 43U);
}

TEST(CompatibilityTest, V13ReaderDefaultsMissingV13Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1_2::CreateCompatibilityProbe(builder, 41U, 42U, 43U);
  v1_2::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_3::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_3::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->third_additive_value(), 0U);
}

TEST(CompatibilityTest, V13ReaderIgnoresV14Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1_4::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U, 45U);
  v1_4::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_3::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_3::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->third_additive_value(), 44U);
}

TEST(CompatibilityTest, V14ReaderDefaultsMissingV14Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1_3::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U);
  v1_3::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_4::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_4::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->fourth_additive_value(), 0U);
}

TEST(CompatibilityTest, V14ReaderIgnoresV15Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe =
      v1_5::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U, 45U, 46U);
  v1_5::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_4::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_4::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->fourth_additive_value(), 45U);
}

TEST(CompatibilityTest, V15ReaderDefaultsMissingV15Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe = v1_4::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U, 45U);
  v1_4::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_5::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_5::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->fifth_additive_value(), 0U);
}

TEST(CompatibilityTest, V15ReaderIgnoresV16Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe =
      v1_6::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U, 45U, 46U, 47U);
  v1_6::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_5::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_5::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->fifth_additive_value(), 46U);
}

TEST(CompatibilityTest, V16ReaderDefaultsMissingV16Field) {
  flatbuffers::FlatBufferBuilder builder;
  const auto probe =
      v1_5::CreateCompatibilityProbe(builder, 41U, 42U, 43U, 44U, 45U, 46U);
  v1_5::FinishCompatibilityProbeBuffer(builder, probe);
  flatbuffers::Verifier verifier{builder.GetBufferPointer(), builder.GetSize()};
  ASSERT_TRUE(v1_6::VerifyCompatibilityProbeBuffer(verifier));
  const auto* decoded = v1_6::GetCompatibilityProbe(builder.GetBufferPointer());
  EXPECT_EQ(decoded->stable_value(), 41U);
  EXPECT_EQ(decoded->sixth_additive_value(), 0U);
}

} // namespace
