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

#include "rpc.h"
#include "capability-context.h"
#include "test-util.h"
#include "schema.h"
#include <kj/debug.h>
#include <kj/string-tree.h>
#include <gtest/gtest.h>
#include <capnp/rpc.capnp.h>
#include <map>
#include <queue>

namespace capnp {
namespace _ {  // private
namespace {

class RpcDumper {
  // Class which stringifies RPC messages for debugging purposes, including decoding params and
  // results based on the call's interface and method IDs and extracting cap descriptors.

public:
  void addSchema(InterfaceSchema schema) {
    schemas[schema.getProto().getId()] = schema;
  }

  enum Sender {
    CLIENT,
    SERVER
  };

  kj::String dump(rpc::Message::Reader message, Sender sender) {
    const char* senderName = sender == CLIENT ? "client" : "server";

    switch (message.which()) {
      case rpc::Message::CALL: {
        auto call = message.getCall();
        auto iter = schemas.find(call.getInterfaceId());
        if (iter == schemas.end()) {
          break;
        }
        InterfaceSchema schema = iter->second;
        auto methods = schema.getMethods();
        if (call.getMethodId() >= methods.size()) {
          break;
        }
        InterfaceSchema::Method method = methods[call.getMethodId()];

        auto schemaProto = schema.getProto();
        auto interfaceName =
            schemaProto.getDisplayName().slice(schemaProto.getDisplayNamePrefixLength());

        auto methodProto = method.getProto();
        auto paramType = schema.getDependency(methodProto.getParamStructType()).asStruct();
        auto resultType = schema.getDependency(methodProto.getResultStructType()).asStruct();

        if (call.getSendResultsTo().isCaller()) {
          returnTypes[std::make_pair(sender, call.getQuestionId())] = resultType;
        }

        CapExtractorImpl extractor;
        CapReaderContext context(extractor);

        auto params = kj::str(context.imbue(call.getParams()).getAs<DynamicStruct>(paramType));

        auto sendResultsTo = call.getSendResultsTo();

        return kj::str(senderName, "(", call.getQuestionId(), "): call ",
                       call.getTarget(), " <- ", interfaceName, ".",
                       methodProto.getName(), params,
                       " caps:[", extractor.printCaps(), "]",
                       sendResultsTo.isCaller() ? kj::str()
                                                : kj::str(" sendResultsTo:", sendResultsTo));
      }

      case rpc::Message::RETURN: {
        auto ret = message.getReturn();

        auto iter = returnTypes.find(
            std::make_pair(sender == CLIENT ? SERVER : CLIENT, ret.getQuestionId()));
        if (iter == returnTypes.end()) {
          break;
        }

        auto schema = iter->second;
        returnTypes.erase(iter);
        if (ret.which() != rpc::Return::RESULTS) {
          // Oops, no results returned.  We don't check this earlier because we want to make sure
          // returnTypes.erase() gets a chance to happen.
          break;
        }

        CapExtractorImpl extractor;
        CapReaderContext context(extractor);
        auto imbued = context.imbue(ret.getResults());

        if (schema.getProto().isStruct()) {
          auto results = kj::str(imbued.getAs<DynamicStruct>(schema.asStruct()));

          return kj::str(senderName, "(", ret.getQuestionId(), "): return ", results,
                         " caps:[", extractor.printCaps(), "]");
        } else if (schema.getProto().isInterface()) {
          imbued.getAs<DynamicCapability>(schema.asInterface());
          return kj::str(senderName, "(", ret.getQuestionId(), "): return cap ",
                         extractor.printCaps());
        } else {
          break;
        }
      }

      case rpc::Message::RESTORE: {
        auto restore = message.getRestore();

        returnTypes[std::make_pair(sender, restore.getQuestionId())] = InterfaceSchema();

        return kj::str(senderName, "(", restore.getQuestionId(), "): restore ",
                       restore.getObjectId().getAs<test::TestSturdyRefObjectId>());
      }

      default:
        break;
    }

    return kj::str(senderName, ": ", message);
  }

private:
  std::map<uint64_t, InterfaceSchema> schemas;
  std::map<std::pair<Sender, uint32_t>, Schema> returnTypes;

  class CapExtractorImpl: public CapExtractor<rpc::CapDescriptor> {
  public:
    kj::Own<ClientHook> extractCap(rpc::CapDescriptor::Reader descriptor) override {
      caps.add(kj::str(descriptor));
      return newBrokenCap("fake cap");
    }

    kj::String printCaps() {
      return kj::strArray(caps, ", ");
    }

  private:
    kj::Vector<kj::String> caps;
  };
};

// =======================================================================================

class TestNetworkAdapter;

class TestNetwork {
public:
  TestNetwork() {
    dumper.addSchema(Schema::from<test::TestInterface>());
    dumper.addSchema(Schema::from<test::TestExtends>());
    dumper.addSchema(Schema::from<test::TestPipeline>());
    dumper.addSchema(Schema::from<test::TestCallOrder>());
    dumper.addSchema(Schema::from<test::TestTailCallee>());
    dumper.addSchema(Schema::from<test::TestTailCaller>());
    dumper.addSchema(Schema::from<test::TestMoreStuff>());
  }
  ~TestNetwork() noexcept(false);

  TestNetworkAdapter& add(kj::StringPtr name);

  kj::Maybe<TestNetworkAdapter&> find(kj::StringPtr name) {
    auto iter = map.find(name);
    if (iter == map.end()) {
      return nullptr;
    } else {
      return *iter->second;
    }
  }

  RpcDumper dumper;

private:
  std::map<kj::StringPtr, kj::Own<TestNetworkAdapter>> map;
};

typedef VatNetwork<
    test::TestSturdyRefHostId, test::TestProvisionId, test::TestRecipientId,
    test::TestThirdPartyCapId, test::TestJoinResult> TestNetworkAdapterBase;

class TestNetworkAdapter final: public TestNetworkAdapterBase {
public:
  TestNetworkAdapter(TestNetwork& network): network(network) {}

  ~TestNetworkAdapter() {
    kj::Exception exception(
        kj::Exception::Nature::PRECONDITION, kj::Exception::Durability::PERMANENT,
        __FILE__, __LINE__, kj::str("Network was destroyed."));
    for (auto& entry: connections) {
      entry.second->disconnect(kj::cp(exception));
    }
  }

  uint getSentCount() { return sent; }
  uint getReceivedCount() { return received; }

  typedef TestNetworkAdapterBase::Connection Connection;

  class ConnectionImpl final
      : public Connection, public kj::Refcounted, public kj::TaskSet::ErrorHandler {
  public:
    ConnectionImpl(TestNetworkAdapter& network, RpcDumper::Sender sender)
        : network(network), sender(sender), tasks(kj::heap<kj::TaskSet>(*this)) {}

    void attach(ConnectionImpl& other) {
      KJ_REQUIRE(partner == nullptr);
      KJ_REQUIRE(other.partner == nullptr);
      partner = other;
      other.partner = *this;
    }

    void disconnect(kj::Exception&& exception) {
      while (!fulfillers.empty()) {
        fulfillers.front()->reject(kj::cp(exception));
        fulfillers.pop();
      }

      networkException = kj::mv(exception);

      tasks = nullptr;
    }

    class IncomingRpcMessageImpl final: public IncomingRpcMessage, public kj::Refcounted {
    public:
      IncomingRpcMessageImpl(uint firstSegmentWordSize)
          : message(firstSegmentWordSize == 0 ? SUGGESTED_FIRST_SEGMENT_WORDS
                                              : firstSegmentWordSize) {}

      ObjectPointer::Reader getBody() override {
        return message.getRoot<ObjectPointer>().asReader();
      }

      MallocMessageBuilder message;
    };

    class OutgoingRpcMessageImpl final: public OutgoingRpcMessage {
    public:
      OutgoingRpcMessageImpl(ConnectionImpl& connection, uint firstSegmentWordSize)
          : connection(connection),
            message(kj::refcounted<IncomingRpcMessageImpl>(firstSegmentWordSize)) {}

      ObjectPointer::Builder getBody() override {
        return message->message.getRoot<ObjectPointer>();
      }
      void send() override {
        if (connection.networkException != nullptr) {
          return;
        }

        ++connection.network.sent;

        // Uncomment to get a debug dump.
//        kj::String msg = connection.network.network.dumper.dump(
//            message->message.getRoot<rpc::Message>(), connection.sender);
//        KJ_ DBG(msg);

        auto connectionPtr = &connection;
        connection.tasks->add(kj::evalLater(kj::mvCapture(kj::addRef(*message),
            [connectionPtr](kj::Own<IncomingRpcMessageImpl>&& message) {
          KJ_IF_MAYBE(p, connectionPtr->partner) {
            if (p->fulfillers.empty()) {
              p->messages.push(kj::mv(message));
            } else {
              ++p->network.received;
              p->fulfillers.front()->fulfill(
                  kj::Own<IncomingRpcMessage>(kj::mv(message)));
              p->fulfillers.pop();
            }
          }
        })));
      }

    private:
      ConnectionImpl& connection;
      kj::Own<IncomingRpcMessageImpl> message;
    };

    kj::Own<OutgoingRpcMessage> newOutgoingMessage(uint firstSegmentWordSize) override {
      return kj::heap<OutgoingRpcMessageImpl>(*this, firstSegmentWordSize);
    }
    kj::Promise<kj::Maybe<kj::Own<IncomingRpcMessage>>> receiveIncomingMessage() override {
      KJ_IF_MAYBE(e, networkException) {
        return kj::cp(*e);
      }

      if (messages.empty()) {
        auto paf = kj::newPromiseAndFulfiller<kj::Maybe<kj::Own<IncomingRpcMessage>>>();
        fulfillers.push(kj::mv(paf.fulfiller));
        return kj::mv(paf.promise);
      } else {
        ++network.received;
        auto result = kj::mv(messages.front());
        messages.pop();
        return kj::Maybe<kj::Own<IncomingRpcMessage>>(kj::mv(result));
      }
    }
    void introduceTo(Connection& recipient,
        test::TestThirdPartyCapId::Builder sendToRecipient,
        test::TestRecipientId::Builder sendToTarget) override {
      KJ_FAIL_ASSERT("not implemented");
    }
    ConnectionAndProvisionId connectToIntroduced(
        test::TestThirdPartyCapId::Reader capId) override {
      KJ_FAIL_ASSERT("not implemented");
    }
    kj::Own<Connection> acceptIntroducedConnection(
        test::TestRecipientId::Reader recipientId) override {
      KJ_FAIL_ASSERT("not implemented");
    }

    void taskFailed(kj::Exception&& exception) override {
      ADD_FAILURE() << kj::str(exception).cStr();
    }

  private:
    TestNetworkAdapter& network;
    RpcDumper::Sender sender KJ_UNUSED_MEMBER;
    kj::Maybe<ConnectionImpl&> partner;

    kj::Maybe<kj::Exception> networkException;

    std::queue<kj::Own<kj::PromiseFulfiller<kj::Maybe<kj::Own<IncomingRpcMessage>>>>> fulfillers;
    std::queue<kj::Own<IncomingRpcMessage>> messages;

    kj::Own<kj::TaskSet> tasks;
  };

  kj::Maybe<kj::Own<Connection>> connectToRefHost(
      test::TestSturdyRefHostId::Reader hostId) override {
    TestNetworkAdapter& dst = KJ_REQUIRE_NONNULL(network.find(hostId.getHost()));

    auto iter = connections.find(&dst);
    if (iter == connections.end()) {
      auto local = kj::refcounted<ConnectionImpl>(*this, RpcDumper::CLIENT);
      auto remote = kj::refcounted<ConnectionImpl>(dst, RpcDumper::SERVER);
      local->attach(*remote);

      connections[&dst] = kj::addRef(*local);
      dst.connections[this] = kj::addRef(*remote);

      if (dst.fulfillerQueue.empty()) {
        dst.connectionQueue.push(kj::mv(remote));
      } else {
        dst.fulfillerQueue.front()->fulfill(kj::mv(remote));
        dst.fulfillerQueue.pop();
      }

      return kj::Own<Connection>(kj::mv(local));
    } else {
      return kj::Own<Connection>(kj::addRef(*iter->second));
    }
  }

  kj::Promise<kj::Own<Connection>> acceptConnectionAsRefHost() override {
    if (connectionQueue.empty()) {
      auto paf = kj::newPromiseAndFulfiller<kj::Own<Connection>>();
      fulfillerQueue.push(kj::mv(paf.fulfiller));
      return kj::mv(paf.promise);
    } else {
      auto result = kj::mv(connectionQueue.front());
      connectionQueue.pop();
      return kj::mv(result);
    }
  }

private:
  TestNetwork& network;
  uint sent = 0;
  uint received = 0;

  std::map<const TestNetworkAdapter*, kj::Own<ConnectionImpl>> connections;
  std::queue<kj::Own<kj::PromiseFulfiller<kj::Own<Connection>>>> fulfillerQueue;
  std::queue<kj::Own<Connection>> connectionQueue;
};

TestNetwork::~TestNetwork() noexcept(false) {}

TestNetworkAdapter& TestNetwork::add(kj::StringPtr name) {
  return *(map[name] = kj::heap<TestNetworkAdapter>(*this));
}

// =======================================================================================

class TestRestorer final: public SturdyRefRestorer<test::TestSturdyRefObjectId> {
public:
  int callCount = 0;

  Capability::Client restore(test::TestSturdyRefObjectId::Reader objectId) override {
    switch (objectId.getTag()) {
      case test::TestSturdyRefObjectId::Tag::TEST_INTERFACE:
        return kj::heap<TestInterfaceImpl>(callCount);
      case test::TestSturdyRefObjectId::Tag::TEST_EXTENDS:
        return Capability::Client(newBrokenCap("No TestExtends implemented."));
      case test::TestSturdyRefObjectId::Tag::TEST_PIPELINE:
        return kj::heap<TestPipelineImpl>(callCount);
      case test::TestSturdyRefObjectId::Tag::TEST_TAIL_CALLEE:
        return kj::heap<TestTailCalleeImpl>(callCount);
      case test::TestSturdyRefObjectId::Tag::TEST_TAIL_CALLER:
        return kj::heap<TestTailCallerImpl>(callCount);
      case test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF:
        return kj::heap<TestMoreStuffImpl>(callCount);
    }
    KJ_UNREACHABLE;
  }
};

struct TestContext {
  kj::EventLoop loop;
  kj::WaitScope waitScope;
  TestNetwork network;
  TestRestorer restorer;
  TestNetworkAdapter& clientNetwork;
  TestNetworkAdapter& serverNetwork;
  RpcSystem<test::TestSturdyRefHostId> rpcClient;
  RpcSystem<test::TestSturdyRefHostId> rpcServer;

  TestContext()
      : waitScope(loop),
        clientNetwork(network.add("client")),
        serverNetwork(network.add("server")),
        rpcClient(makeRpcClient(clientNetwork)),
        rpcServer(makeRpcServer(serverNetwork, restorer)) {}

  Capability::Client connect(test::TestSturdyRefObjectId::Tag tag) {
    MallocMessageBuilder refMessage(128);
    auto ref = refMessage.initRoot<rpc::SturdyRef>();
    auto hostId = ref.getHostId().initAs<test::TestSturdyRefHostId>();
    hostId.setHost("server");
    ref.getObjectId().initAs<test::TestSturdyRefObjectId>().setTag(tag);

    return rpcClient.restore(hostId, ref.getObjectId());
  }
};

TEST(Rpc, Basic) {
  TestContext context;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_INTERFACE)
      .castAs<test::TestInterface>();

  auto request1 = client.fooRequest();
  request1.setI(123);
  request1.setJ(true);
  auto promise1 = request1.send();

  // We used to call bar() after baz(), hence the numbering, but this masked the case where the
  // RPC system actually disconnected on bar() (thus returning an exception, which we decided
  // was expected).
  bool barFailed = false;
  auto request3 = client.barRequest();
  auto promise3 = request3.send().then(
      [](Response<test::TestInterface::BarResults>&& response) {
        ADD_FAILURE() << "Expected bar() call to fail.";
      }, [&](kj::Exception&& e) {
        barFailed = true;
      });

  auto request2 = client.bazRequest();
  initTestMessage(request2.initS());
  auto promise2 = request2.send();

  EXPECT_EQ(0, context.restorer.callCount);

  auto response1 = promise1.wait(context.waitScope);

  EXPECT_EQ("foo", response1.getX());

  auto response2 = promise2.wait(context.waitScope);

  promise3.wait(context.waitScope);

  EXPECT_EQ(2, context.restorer.callCount);
  EXPECT_TRUE(barFailed);
}

TEST(Rpc, Pipelining) {
  TestContext context;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_PIPELINE)
      .castAs<test::TestPipeline>();

  int chainedCallCount = 0;

  auto request = client.getCapRequest();
  request.setN(234);
  request.setInCap(kj::heap<TestInterfaceImpl>(chainedCallCount));

  auto promise = request.send();

  auto pipelineRequest = promise.getOutBox().getCap().fooRequest();
  pipelineRequest.setI(321);
  auto pipelinePromise = pipelineRequest.send();

  auto pipelineRequest2 = promise.getOutBox().getCap().castAs<test::TestExtends>().graultRequest();
  auto pipelinePromise2 = pipelineRequest2.send();

  promise = nullptr;  // Just to be annoying, drop the original promise.

  EXPECT_EQ(0, context.restorer.callCount);
  EXPECT_EQ(0, chainedCallCount);

  auto response = pipelinePromise.wait(context.waitScope);
  EXPECT_EQ("bar", response.getX());

  auto response2 = pipelinePromise2.wait(context.waitScope);
  checkTestMessage(response2);

  EXPECT_EQ(3, context.restorer.callCount);
  EXPECT_EQ(1, chainedCallCount);
}

TEST(Rpc, TailCall) {
  TestContext context;

  auto caller = context.connect(test::TestSturdyRefObjectId::Tag::TEST_TAIL_CALLER)
      .castAs<test::TestTailCaller>();

  int calleeCallCount = 0;

  test::TestTailCallee::Client callee(kj::heap<TestTailCalleeImpl>(calleeCallCount));

  auto request = caller.fooRequest();
  request.setI(456);
  request.setCallee(callee);

  auto promise = request.send();

  auto dependentCall0 = promise.getC().getCallSequenceRequest().send();

  auto response = promise.wait(context.waitScope);
  EXPECT_EQ(456, response.getI());
  EXPECT_EQ(456, response.getI());

  auto dependentCall1 = promise.getC().getCallSequenceRequest().send();

  auto dependentCall2 = response.getC().getCallSequenceRequest().send();

  EXPECT_EQ(0, dependentCall0.wait(context.waitScope).getN());
  EXPECT_EQ(1, dependentCall1.wait(context.waitScope).getN());
  EXPECT_EQ(2, dependentCall2.wait(context.waitScope).getN());

  EXPECT_EQ(1, calleeCallCount);
  EXPECT_EQ(1, context.restorer.callCount);
}

TEST(Rpc, AsyncCancelation) {
  // Tests allowAsyncCancellation().

  TestContext context;

  auto paf = kj::newPromiseAndFulfiller<void>();
  bool destroyed = false;
  auto destructionPromise = paf.promise.then([&]() { destroyed = true; }).eagerlyEvaluate(nullptr);

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
      .castAs<test::TestMoreStuff>();

  kj::Promise<void> promise = nullptr;

  bool returned = false;
  {
    auto request = client.expectAsyncCancelRequest();
    request.setCap(kj::heap<TestCapDestructor>(kj::mv(paf.fulfiller)));
    promise = request.send().then(
        [&](Response<test::TestMoreStuff::ExpectAsyncCancelResults>&& response) {
      returned = true;
    }).eagerlyEvaluate(nullptr);
  }
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);

  // We can detect that the method was canceled because it will drop the cap.
  EXPECT_FALSE(destroyed);
  EXPECT_FALSE(returned);

  promise = nullptr;  // request cancellation
  destructionPromise.wait(context.waitScope);

  EXPECT_TRUE(destroyed);
  EXPECT_FALSE(returned);
}

TEST(Rpc, SyncCancelation) {
  // Tests isCanceled() without allowAsyncCancellation().

  TestContext context;

  int innerCallCount = 0;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
      .castAs<test::TestMoreStuff>();

  kj::Promise<void> promise = nullptr;

  bool returned = false;
  {
    auto request = client.expectSyncCancelRequest();
    request.setCap(kj::heap<TestInterfaceImpl>(innerCallCount));
    promise = request.send().then(
        [&](Response<test::TestMoreStuff::ExpectSyncCancelResults>&& response) {
      returned = true;
    }).eagerlyEvaluate(nullptr);
  }
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);

  // expectSyncCancel() will make a call to the TestInterfaceImpl only once it noticed isCanceled()
  // is true.
  EXPECT_EQ(0, innerCallCount);
  EXPECT_FALSE(returned);

  promise = nullptr;  // request cancellation
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);
  kj::evalLater([]() {}).wait(context.waitScope);

  EXPECT_EQ(1, innerCallCount);
  EXPECT_FALSE(returned);
}

TEST(Rpc, PromiseResolve) {
  TestContext context;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
      .castAs<test::TestMoreStuff>();

  int chainedCallCount = 0;

  auto request = client.callFooRequest();
  auto request2 = client.callFooWhenResolvedRequest();

  auto paf = kj::newPromiseAndFulfiller<test::TestInterface::Client>();

  {
    auto fork = paf.promise.fork();
    request.setCap(fork.addBranch());
    request2.setCap(fork.addBranch());
  }

  auto promise = request.send();
  auto promise2 = request2.send();

  // Make sure getCap() has been called on the server side by sending another call and waiting
  // for it.
  EXPECT_EQ(2, client.getCallSequenceRequest().send().wait(context.waitScope).getN());
  EXPECT_EQ(3, context.restorer.callCount);

  // OK, now fulfill the local promise.
  paf.fulfiller->fulfill(kj::heap<TestInterfaceImpl>(chainedCallCount));

  // We should now be able to wait for getCap() to finish.
  EXPECT_EQ("bar", promise.wait(context.waitScope).getS());
  EXPECT_EQ("bar", promise2.wait(context.waitScope).getS());

  EXPECT_EQ(3, context.restorer.callCount);
  EXPECT_EQ(2, chainedCallCount);
}

TEST(Rpc, RetainAndRelease) {
  TestContext context;

  auto paf = kj::newPromiseAndFulfiller<void>();
  bool destroyed = false;
  auto destructionPromise = paf.promise.then([&]() { destroyed = true; }).eagerlyEvaluate(nullptr);

  {
    auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
        .castAs<test::TestMoreStuff>();

    {
      auto request = client.holdRequest();
      request.setCap(kj::heap<TestCapDestructor>(kj::mv(paf.fulfiller)));
      request.send().wait(context.waitScope);
    }

    // Do some other call to add a round trip.
    EXPECT_EQ(1, client.getCallSequenceRequest().send().wait(context.waitScope).getN());

    // Shouldn't be destroyed because it's being held by the server.
    EXPECT_FALSE(destroyed);

    // We can ask it to call the held capability.
    EXPECT_EQ("bar", client.callHeldRequest().send().wait(context.waitScope).getS());

    {
      // We can get the cap back from it.
      auto capCopy = client.getHeldRequest().send().wait(context.waitScope).getCap();

      {
        // And call it, without any network communications.
        uint oldSentCount = context.clientNetwork.getSentCount();
        auto request = capCopy.fooRequest();
        request.setI(123);
        request.setJ(true);
        EXPECT_EQ("foo", request.send().wait(context.waitScope).getX());
        EXPECT_EQ(oldSentCount, context.clientNetwork.getSentCount());
      }

      {
        // We can send another copy of the same cap to another method, and it works.
        auto request = client.callFooRequest();
        request.setCap(capCopy);
        EXPECT_EQ("bar", request.send().wait(context.waitScope).getS());
      }
    }

    // Give some time to settle.
    EXPECT_EQ(5, client.getCallSequenceRequest().send().wait(context.waitScope).getN());
    EXPECT_EQ(6, client.getCallSequenceRequest().send().wait(context.waitScope).getN());
    EXPECT_EQ(7, client.getCallSequenceRequest().send().wait(context.waitScope).getN());

    // Can't be destroyed, we haven't released it.
    EXPECT_FALSE(destroyed);
  }

  // We released our client, which should cause the server to be released, which in turn will
  // release the cap pointing back to us.
  destructionPromise.wait(context.waitScope);
  EXPECT_TRUE(destroyed);
}

TEST(Rpc, Cancel) {
  TestContext context;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
      .castAs<test::TestMoreStuff>();

  auto paf = kj::newPromiseAndFulfiller<void>();
  bool destroyed = false;
  auto destructionPromise = paf.promise.then([&]() { destroyed = true; }).eagerlyEvaluate(nullptr);

  {
    auto request = client.neverReturnRequest();
    request.setCap(kj::heap<TestCapDestructor>(kj::mv(paf.fulfiller)));

    {
      auto responsePromise = request.send();

      // Allow some time to settle.
      EXPECT_EQ(1, client.getCallSequenceRequest().send().wait(context.waitScope).getN());
      EXPECT_EQ(2, client.getCallSequenceRequest().send().wait(context.waitScope).getN());

      // The cap shouldn't have been destroyed yet because the call never returned.
      EXPECT_FALSE(destroyed);
    }
  }

  // Now the cap should be released.
  destructionPromise.wait(context.waitScope);
  EXPECT_TRUE(destroyed);
}

TEST(Rpc, SendTwice) {
  TestContext context;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
      .castAs<test::TestMoreStuff>();

  auto paf = kj::newPromiseAndFulfiller<void>();
  bool destroyed = false;
  auto destructionPromise = paf.promise.then([&]() { destroyed = true; }).eagerlyEvaluate(nullptr);

  auto cap = test::TestInterface::Client(kj::heap<TestCapDestructor>(kj::mv(paf.fulfiller)));

  {
    auto request = client.callFooRequest();
    request.setCap(cap);

    EXPECT_EQ("bar", request.send().wait(context.waitScope).getS());
  }

  // Allow some time for the server to release `cap`.
  EXPECT_EQ(1, client.getCallSequenceRequest().send().wait(context.waitScope).getN());

  {
    // More requests with the same cap.
    auto request = client.callFooRequest();
    auto request2 = client.callFooRequest();
    request.setCap(cap);
    request2.setCap(kj::mv(cap));

    auto promise = request.send();
    auto promise2 = request2.send();

    EXPECT_EQ("bar", promise.wait(context.waitScope).getS());
    EXPECT_EQ("bar", promise2.wait(context.waitScope).getS());
  }

  // Now the cap should be released.
  destructionPromise.wait(context.waitScope);
  EXPECT_TRUE(destroyed);
}

RemotePromise<test::TestCallOrder::GetCallSequenceResults> getCallSequence(
    test::TestCallOrder::Client& client, uint expected) {
  auto req = client.getCallSequenceRequest();
  req.setExpected(expected);
  return req.send();
}

TEST(Rpc, Embargo) {
  TestContext context;

  auto client = context.connect(test::TestSturdyRefObjectId::Tag::TEST_MORE_STUFF)
      .castAs<test::TestMoreStuff>();

  auto cap = test::TestCallOrder::Client(kj::heap<TestCallOrderImpl>());

  auto earlyCall = client.getCallSequenceRequest().send();

  auto echoRequest = client.echoRequest();
  echoRequest.setCap(cap);
  auto echo = echoRequest.send();

  auto pipeline = echo.getCap();

  auto call0 = getCallSequence(pipeline, 0);
  auto call1 = getCallSequence(pipeline, 1);

  earlyCall.wait(context.waitScope);

  auto call2 = getCallSequence(pipeline, 2);

  auto resolved = echo.wait(context.waitScope).getCap();

  auto call3 = getCallSequence(pipeline, 3);
  auto call4 = getCallSequence(pipeline, 4);
  auto call5 = getCallSequence(pipeline, 5);

  EXPECT_EQ(0, call0.wait(context.waitScope).getN());
  EXPECT_EQ(1, call1.wait(context.waitScope).getN());
  EXPECT_EQ(2, call2.wait(context.waitScope).getN());
  EXPECT_EQ(3, call3.wait(context.waitScope).getN());
  EXPECT_EQ(4, call4.wait(context.waitScope).getN());
  EXPECT_EQ(5, call5.wait(context.waitScope).getN());
}

}  // namespace
}  // namespace _ (private)
}  // namespace capnp
