// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "rpc-twoparty.h"
#include "serialize-async.h"
#include <kj/debug.h>

namespace capnp {

TwoPartyVatNetwork::TwoPartyVatNetwork(kj::AsyncIoStream& stream, rpc::twoparty::Side side,
                                       ReaderOptions receiveOptions)
    : stream(stream), side(side), receiveOptions(receiveOptions), previousWrite(kj::READY_NOW) {
  {
    auto paf = kj::newPromiseAndFulfiller<void>();
    disconnectPromise = paf.promise.fork();
    disconnectFulfiller = kj::mv(paf.fulfiller);
  }
  {
    auto paf = kj::newPromiseAndFulfiller<void>();
    drainedPromise = paf.promise.fork();
    drainedFulfiller.fulfiller = kj::mv(paf.fulfiller);
  }
}

kj::Maybe<kj::Own<TwoPartyVatNetworkBase::Connection>> TwoPartyVatNetwork::connectToRefHost(
    rpc::twoparty::SturdyRefHostId::Reader ref) {
  if (ref.getSide() == side) {
    return nullptr;
  } else {
    return kj::Own<TwoPartyVatNetworkBase::Connection>(this, drainedFulfiller);
  }
}

kj::Promise<kj::Own<TwoPartyVatNetworkBase::Connection>>
    TwoPartyVatNetwork::acceptConnectionAsRefHost() {
  if (side == rpc::twoparty::Side::SERVER && !accepted) {
    accepted = true;
    return kj::Own<TwoPartyVatNetworkBase::Connection>(this,
        kj::DestructorOnlyDisposer<TwoPartyVatNetworkBase::Connection>::instance);
  } else {
    // Create a promise that will never be fulfilled.
    auto paf = kj::newPromiseAndFulfiller<kj::Own<TwoPartyVatNetworkBase::Connection>>();
    acceptFulfiller = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }
}

class TwoPartyVatNetwork::OutgoingMessageImpl final
    : public OutgoingRpcMessage, public kj::Refcounted {
public:
  OutgoingMessageImpl(TwoPartyVatNetwork& network, uint firstSegmentWordSize)
      : network(network),
        message(firstSegmentWordSize == 0 ? SUGGESTED_FIRST_SEGMENT_WORDS : firstSegmentWordSize) {}

  ObjectPointer::Builder getBody() override {
    return message.getRoot<ObjectPointer>();
  }

  void send() override {
    network.previousWrite = network.previousWrite.then([&]() {
      auto promise = writeMessage(network.stream, message).then([]() {
        // success; do nothing
      }, [&](kj::Exception&& exception) {
        // Exception during write!
        network.disconnectFulfiller->fulfill();
      }).eagerlyEvaluate(nullptr);
      return kj::mv(promise);
    }).attach(kj::addRef(*this));
  }

private:
  TwoPartyVatNetwork& network;
  MallocMessageBuilder message;
};

class TwoPartyVatNetwork::IncomingMessageImpl final: public IncomingRpcMessage {
public:
  IncomingMessageImpl(kj::Own<MessageReader> message): message(kj::mv(message)) {}

  ObjectPointer::Reader getBody() override {
    return message->getRoot<ObjectPointer>();
  }

private:
  kj::Own<MessageReader> message;
};

kj::Own<OutgoingRpcMessage> TwoPartyVatNetwork::newOutgoingMessage(uint firstSegmentWordSize) {
  return kj::refcounted<OutgoingMessageImpl>(*this, firstSegmentWordSize);
}

kj::Promise<kj::Maybe<kj::Own<IncomingRpcMessage>>> TwoPartyVatNetwork::receiveIncomingMessage() {
  return kj::evalLater([&]() {
    return tryReadMessage(stream, receiveOptions)
        .then([&](kj::Maybe<kj::Own<MessageReader>>&& message)
              -> kj::Maybe<kj::Own<IncomingRpcMessage>> {
      KJ_IF_MAYBE(m, message) {
        return kj::Own<IncomingRpcMessage>(kj::heap<IncomingMessageImpl>(kj::mv(*m)));
      } else {
        disconnectFulfiller->fulfill();
        return nullptr;
      }
    }, [&](kj::Exception&& exception) {
      disconnectFulfiller->fulfill();
      kj::throwRecoverableException(kj::mv(exception));
      return nullptr;
    });
  });
}

void TwoPartyVatNetwork::introduceTo(TwoPartyVatNetworkBase::Connection& recipient,
    rpc::twoparty::ThirdPartyCapId::Builder sendToRecipient,
    rpc::twoparty::RecipientId::Builder sendToTarget) {
  KJ_FAIL_REQUIRE("Three-party introductions should never occur on two-party network.");
}

TwoPartyVatNetworkBase::ConnectionAndProvisionId TwoPartyVatNetwork::connectToIntroduced(
    rpc::twoparty::ThirdPartyCapId::Reader capId) {
  KJ_FAIL_REQUIRE("Three-party introductions should never occur on two-party network.");
}

kj::Own<TwoPartyVatNetworkBase::Connection> TwoPartyVatNetwork::acceptIntroducedConnection(
    rpc::twoparty::RecipientId::Reader recipientId) {
  KJ_FAIL_REQUIRE("Three-party introductions should never occur on two-party network.");
}

}  // namespace capnp
