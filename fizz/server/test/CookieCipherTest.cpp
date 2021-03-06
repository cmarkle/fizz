/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fizz/server/CookieCipher.h>

#include <fizz/protocol/test/Mocks.h>
#include <fizz/protocol/test/TestMessages.h>

using namespace fizz::test;
using namespace folly;
using namespace testing;

static constexpr StringPiece statelessHrr{
    "020000380303cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c001301000010002b00020304002c0006000474657374"};

static constexpr StringPiece statelessHrrGroup{
    "0200003e0303cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c001301000016002b0002030400330002001d002c0006000474657374"};

namespace fizz {
namespace server {
namespace test {

TEST(GetStatelessHrrTest, NoGroup) {
  auto hrr = getStatelessHelloRetryRequest(
      ProtocolVersion::tls_1_3,
      CipherSuite::TLS_AES_128_GCM_SHA256,
      none,
      IOBuf::copyBuffer("test"));
  EXPECT_EQ(hexlify(hrr->coalesce()), statelessHrr);
}

TEST(GetStatelessHrrTest, Group) {
  auto hrr = getStatelessHelloRetryRequest(
      ProtocolVersion::tls_1_3,
      CipherSuite::TLS_AES_128_GCM_SHA256,
      NamedGroup::x25519,
      IOBuf::copyBuffer("test"));
  EXPECT_EQ(hexlify(hrr->coalesce()), statelessHrrGroup);
}

class GetCookieStateTest : public Test {
 public:
  void SetUp() override {
    factory_.setDefaults();
  }

 protected:
  MockFactory factory_;
};

TEST_F(GetCookieStateTest, TestBasic) {
  auto mockHandshakeContext = new MockHandshakeContext();
  Sequence seq;
  EXPECT_CALL(
      factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .InSequence(seq)
      .WillOnce(InvokeWithoutArgs([=]() {
        return std::unique_ptr<HandshakeContext>(mockHandshakeContext);
      }));
  EXPECT_CALL(
      *mockHandshakeContext,
      appendToTranscript(BufMatches("clienthelloencoding")))
      .InSequence(seq);
  EXPECT_CALL(*mockHandshakeContext, getHandshakeContext())
      .WillOnce(Invoke([]() { return IOBuf::copyBuffer("context"); }));
  auto state = getCookieState(
      factory_,
      {TestProtocolVersion},
      {{CipherSuite::TLS_AES_128_GCM_SHA256}},
      {NamedGroup::x25519},
      TestMessages::clientHello(),
      IOBuf::copyBuffer("token"));
  EXPECT_EQ(state.version, TestProtocolVersion);
  EXPECT_EQ(state.cipher, CipherSuite::TLS_AES_128_GCM_SHA256);
  EXPECT_FALSE(state.group.hasValue());
  EXPECT_TRUE(IOBufEqualTo()(state.chloHash, IOBuf::copyBuffer("context")));
  EXPECT_TRUE(IOBufEqualTo()(state.appToken, IOBuf::copyBuffer("token")));
}

TEST_F(GetCookieStateTest, TestNoVersion) {
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::supported_versions);
  EXPECT_THROW(
      getCookieState(
          factory_,
          {TestProtocolVersion},
          {{CipherSuite::TLS_AES_128_GCM_SHA256}},
          {NamedGroup::x25519},
          chlo,
          IOBuf::copyBuffer("token")),
      std::runtime_error);
}

TEST_F(GetCookieStateTest, TestVersionMismatch) {
  EXPECT_THROW(
      getCookieState(
          factory_,
          {ProtocolVersion::tls_1_2},
          {{CipherSuite::TLS_AES_128_GCM_SHA256}},
          {NamedGroup::x25519},
          TestMessages::clientHello(),
          IOBuf::copyBuffer("token")),
      std::runtime_error);
}

TEST_F(GetCookieStateTest, TestCipherNegotiate) {
  EXPECT_CALL(
      factory_, makeHandshakeContext(CipherSuite::TLS_AES_256_GCM_SHA384))
      .WillOnce(InvokeWithoutArgs([=]() {
        auto ret = std::make_unique<MockHandshakeContext>();
        ret->setDefaults();
        return ret;
      }));
  auto state = getCookieState(
      factory_,
      {TestProtocolVersion},
      {{CipherSuite::TLS_AES_256_GCM_SHA384},
       {CipherSuite::TLS_AES_128_GCM_SHA256}},
      {NamedGroup::x25519},
      TestMessages::clientHello(),
      IOBuf::copyBuffer("token"));
  EXPECT_EQ(state.cipher, CipherSuite::TLS_AES_256_GCM_SHA384);
}

TEST_F(GetCookieStateTest, TestCipherNegotiateTie) {
  EXPECT_CALL(
      factory_, makeHandshakeContext(CipherSuite::TLS_AES_128_GCM_SHA256))
      .WillOnce(InvokeWithoutArgs([=]() {
        auto ret = std::make_unique<MockHandshakeContext>();
        ret->setDefaults();
        return ret;
      }));
  auto state = getCookieState(
      factory_,
      {TestProtocolVersion},
      {{CipherSuite::TLS_AES_256_GCM_SHA384,
        CipherSuite::TLS_AES_128_GCM_SHA256}},
      {NamedGroup::x25519},
      TestMessages::clientHello(),
      IOBuf::copyBuffer("token"));
  EXPECT_EQ(state.cipher, CipherSuite::TLS_AES_128_GCM_SHA256);
}

TEST_F(GetCookieStateTest, TestCipherMismatch) {
  EXPECT_THROW(
      getCookieState(
          factory_,
          {TestProtocolVersion},
          {{}},
          {NamedGroup::x25519},
          TestMessages::clientHello(),
          IOBuf::copyBuffer("token")),
      std::runtime_error);
}

TEST_F(GetCookieStateTest, TestGroupNotSent) {
  auto state = getCookieState(
      factory_,
      {TestProtocolVersion},
      {{CipherSuite::TLS_AES_128_GCM_SHA256}},
      {NamedGroup::secp256r1},
      TestMessages::clientHello(),
      IOBuf::copyBuffer("token"));
  EXPECT_EQ(*state.group, NamedGroup::secp256r1);
}

TEST_F(GetCookieStateTest, TestNoGroups) {
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::supported_groups);
  TestMessages::removeExtension(chlo, ExtensionType::key_share);
  auto state = getCookieState(
      factory_,
      {TestProtocolVersion},
      {{CipherSuite::TLS_AES_128_GCM_SHA256}},
      {NamedGroup::x25519},
      chlo,
      IOBuf::copyBuffer("token"));
  EXPECT_FALSE(state.group.hasValue());
}

TEST_F(GetCookieStateTest, TestNoKeyShare) {
  auto chlo = TestMessages::clientHello();
  TestMessages::removeExtension(chlo, ExtensionType::key_share);
  EXPECT_THROW(
      getCookieState(
          factory_,
          {TestProtocolVersion},
          {{CipherSuite::TLS_AES_128_GCM_SHA256}},
          {NamedGroup::x25519},
          chlo,
          IOBuf::copyBuffer("token")),
      std::runtime_error);
}

TEST_F(GetCookieStateTest, TestGroupMismatch) {
  auto state = getCookieState(
      factory_,
      {TestProtocolVersion},
      {{CipherSuite::TLS_AES_128_GCM_SHA256}},
      {},
      TestMessages::clientHello(),
      IOBuf::copyBuffer("token"));
  EXPECT_FALSE(state.group.hasValue());
}
} // namespace test
} // namespace server
} // namespace fizz
