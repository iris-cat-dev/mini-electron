// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <type_traits>

#include "base/run_loop.h"
#include "base/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_executor.h"
#include "base/threading/sequence_bound.h"
#include "base/threading/thread.h"
#include "mojo/public/c/system/buffer.h"
#include "mojo/public/c/system/data_pipe.h"
#include "mojo/public/c/system/functions.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/c/system/trap.h"
#include "mojo/public/cpp/bindings/message.h"
#include "mojo/public/cpp/mojo_lite_msg.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/wait.h"
#include "mojo/public/cpp/bindings/urgent_message_scope.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "base/containers/stack_container.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/data_pipe_getter.mojom-blink.h"
#include "services/network/public/mojom/chunked_data_pipe_getter.mojom-blink.h"
#include "runtime/network/loader/flatten_http_body_element.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"

namespace {
void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

void Result(MojoResult actual, MojoResult expected, const char* message) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL: %s (actual %d, expected %d)\n", message,
                 static_cast<int>(actual), static_cast<int>(expected));
    std::abort();
  }
}


struct TrapState {
  unsigned ready = 0;
  unsigned cancelled = 0;
  MojoResult result = MOJO_RESULT_UNKNOWN;
  MojoHandleSignalsState signals = {};
};

void TrapEvent(const MojoTrapEvent* event) {
  auto* state = reinterpret_cast<TrapState*>(event->trigger_context);
  if (event->result == MOJO_RESULT_CANCELLED) {
    ++state->cancelled;
    return;
  }
  ++state->ready;
  state->result = event->result;
  state->signals = event->signals_state;
}

void Arm(MojoHandle trap) {
  uint32_t count = 1;
  MojoTrapEvent event = {sizeof(event)};
  Result(MojoArmTrap(trap, nullptr, &count, &event), MOJO_RESULT_OK, "arm inactive signal trap");
}

void DataPipesAndTraps() {
  const MojoCreateDataPipeOptions options = {sizeof(options), MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1, 4};
  MojoHandle producer = 0, consumer = 0;
  Result(MojoCreateDataPipe(&options, &producer, &consumer), MOJO_RESULT_OK, "create bounded data pipe");
  char bytes[8] = {};
  uint32_t size = 1;
  Result(MojoReadData(consumer, nullptr, bytes, &size), MOJO_RESULT_SHOULD_WAIT,
         "empty data pipe waits");
  const MojoWriteDataOptions all = {sizeof(all), MOJO_WRITE_DATA_FLAG_ALL_OR_NONE};
  size = 5;
  Result(MojoWriteData(producer, "abcde", &size, &all), MOJO_RESULT_OUT_OF_RANGE,
         "all-or-none honors capacity");

  TrapState state;
  MojoHandle trap = 0;
  Result(MojoCreateTrap(&TrapEvent, nullptr, &trap), MOJO_RESULT_OK, "create real numeric trap");
  Result(MojoAddTrigger(trap, consumer, MOJO_HANDLE_SIGNAL_READABLE,
                        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                        reinterpret_cast<uintptr_t>(&state), nullptr),
         MOJO_RESULT_OK, "watch consumer readability");
  Arm(trap);
  size = 4;
  Result(MojoWriteData(producer, "abcd", &size, nullptr), MOJO_RESULT_OK, "fill bounded pipe");
  Require(size == 4, "full write length");
  base::RunLoop().RunUntilIdle();
  Require(state.ready == 1 && state.result == MOJO_RESULT_OK &&
              (state.signals.satisfied_signals & MOJO_HANDLE_SIGNAL_READABLE),
          "write produces actual readable trap callback");
  size = 1;
  Result(MojoWriteData(producer, "x", &size, nullptr), MOJO_RESULT_SHOULD_WAIT,
         "full pipe does not grow beyond capacity");
  uint32_t blocked_count = 1;
  MojoTrapEvent blocked = {sizeof(blocked)};
  Result(MojoArmTrap(trap, nullptr, &blocked_count, &blocked), MOJO_RESULT_FAILED_PRECONDITION,
         "ready trap reports a blocking event instead of arming");
  Require(blocked_count == 1 && blocked.result == MOJO_RESULT_OK,
          "blocking event carries actual readiness");
  size = 2;
  Result(MojoReadData(consumer, nullptr, bytes, &size), MOJO_RESULT_OK, "consume prefix");
  Require(size == 2 && std::memcmp(bytes, "ab", 2) == 0, "data prefix bytes");
  size = 2;
  Result(MojoWriteData(producer, "ef", &size, &all), MOJO_RESULT_OK, "wrap ring write");
  size = 4;
  Result(MojoReadData(consumer, nullptr, bytes, &size), MOJO_RESULT_OK, "read wrapped ring");
  Require(size == 4 && std::memcmp(bytes, "cdef", 4) == 0, "ring preserves wrapped order");

  void* writable = nullptr;
  uint32_t writable_size = 0;
  Result(MojoBeginWriteData(producer, nullptr, &writable, &writable_size),
         MOJO_RESULT_OK, "two-phase write exposes actual ring storage");
  Require(writable_size > 0 && writable_size <= 4, "two-phase write bounded capacity");
  static_cast<char*>(writable)[0] = 'z';
  Result(MojoEndWriteData(producer, 1, nullptr), MOJO_RESULT_OK, "commit two-phase write");
  Result(MojoClose(producer), MOJO_RESULT_OK, "close producer with buffered data");
  const void* readable = nullptr;
  uint32_t readable_size = 0;
  Result(MojoBeginReadData(consumer, nullptr, &readable, &readable_size),
         MOJO_RESULT_OK, "closed producer data remains drainable");
  Require(readable_size == 1 && static_cast<const char*>(readable)[0] == 'z',
          "drain retains final committed byte");
  Result(MojoEndReadData(consumer, 1, nullptr), MOJO_RESULT_OK, "drain final byte");
  blocked_count = 1;
  blocked = {sizeof(blocked)};
  Result(MojoArmTrap(trap, nullptr, &blocked_count, &blocked), MOJO_RESULT_FAILED_PRECONDITION,
         "closed/drained readable signal is unsatisfiable");
  Require(blocked.result == MOJO_RESULT_FAILED_PRECONDITION &&
              (blocked.signals_state.satisfied_signals & MOJO_HANDLE_SIGNAL_PEER_CLOSED),
          "closed/drained trap reports peer-close state");
  size = 1;
  Result(MojoReadData(consumer, nullptr, bytes, &size), MOJO_RESULT_FAILED_PRECONDITION,
         "drained data pipe terminates after producer close");
  Result(MojoClose(trap), MOJO_RESULT_OK, "close trap cancels registration");
  Require(state.cancelled == 1, "trap cancellation delivered exactly once");
  Result(MojoClose(consumer), MOJO_RESULT_OK, "close drained consumer");
  base::RunLoop().RunUntilIdle();

  Result(MojoCreateDataPipe(&options, &producer, &consumer), MOJO_RESULT_OK,
         "create peer-close callback pipe");
  TrapState peer;
  Result(MojoCreateTrap(&TrapEvent, nullptr, &trap), MOJO_RESULT_OK, "create peer-close trap");
  Result(MojoAddTrigger(trap, consumer, MOJO_HANDLE_SIGNAL_PEER_CLOSED,
                        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                        reinterpret_cast<uintptr_t>(&peer), nullptr),
         MOJO_RESULT_OK, "watch peer-close signal");
  Arm(trap);
  Result(MojoClose(producer), MOJO_RESULT_OK, "close watched producer");
  base::RunLoop().RunUntilIdle();
  Require(peer.ready == 1 && peer.result == MOJO_RESULT_OK &&
              (peer.signals.satisfied_signals & MOJO_HANDLE_SIGNAL_PEER_CLOSED),
          "peer close produces actual trap callback");
  Result(MojoClose(trap), MOJO_RESULT_OK, "close peer trap");
  Require(peer.cancelled == 1, "peer trap cancels once");
  Result(MojoClose(consumer), MOJO_RESULT_OK, "close peer consumer");
}

void SharedBufferCapability() {
  MojoHandle output = 0xabcdefu;
  Result(MojoCreateSharedBuffer(64, nullptr, &output), MOJO_RESULT_UNIMPLEMENTED,
         "shared buffer creation reports unsupported capability");
  Require(output == 0xabcdefu, "unsupported create does not invent a handle");
  MojoHandle left = 0, right = 0;
  Result(MojoCreateMessagePipe(nullptr, &left, &right), MOJO_RESULT_OK, "create preserved handle");
  Result(MojoDuplicateBufferHandle(left, nullptr, &output), MOJO_RESULT_UNIMPLEMENTED,
         "shared buffer duplication reports unsupported capability");
  Require(output == 0xabcdefu, "unsupported duplication preserves output");
  char sentinel = 0;
  void* mapping = &sentinel;
  Result(MojoMapBuffer(left, 0, 16, nullptr, &mapping), MOJO_RESULT_UNIMPLEMENTED,
         "mapping reports unsupported capability");
  Require(mapping == &sentinel, "unsupported map does not manufacture storage");
  MojoSharedBufferInfo info = {sizeof(info), 123};
  Result(MojoGetBufferInfo(left, nullptr, &info), MOJO_RESULT_UNIMPLEMENTED,
         "buffer info reports unsupported capability");
  Require(info.size == 123, "unsupported info preserves output");
  Result(MojoUnmapBuffer(mapping), MOJO_RESULT_UNIMPLEMENTED,
         "unmap reports unsupported capability without touching memory");
  MojoHandleSignalsState state = {};
  Result(MojoQueryHandleSignalsState(left, &state), MOJO_RESULT_OK,
         "unsupported buffer operations preserve existing pipe ownership");
  Result(MojoClose(left), MOJO_RESULT_OK, "close preserved left handle");
  Result(MojoClose(right), MOJO_RESULT_OK, "close preserved right handle");
}

void CppMessageHandoff() {
  const uint8_t payload[] = {1, 2, 3};
  mojo::Message message{base::span<const uint8_t>(payload), base::span<mojo::ScopedHandle>()};
  Require(!message.IsNull(), "real C++ payload message is non-null");
  auto raw = message.TakeMojoMessage();
  Require(raw.is_valid() && message.IsNull(), "C++ handoff transfers storage ownership");
  auto received = mojo::Message::CreateFromMessageHandle(&raw);
  Require(!raw.is_valid() && !received.IsNull() && received.data_num_bytes() == 3 &&
              std::memcmp(received.data(), payload, 3) == 0,
          "C++ reconstruction adopts real raw message payload");
  received.Reset();
  Require(received.IsNull(), "C++ reset releases owned payload");
  mojo::Message header(42, mojo::Message::kFlagExpectsResponse, 0, 0, nullptr);
  Require(header.name() == 42 && header.version() == 1 &&
              header.has_flag(mojo::Message::kFlagExpectsResponse),
          "C++ header construction preserves message fields");
}

class URLClientProbe final : public network::mojom::URLLoaderClient {
 public:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr) override { events.push_back(5); }
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr,
                         mojo::ScopedDataPipeConsumerHandle,
                         std::optional<mojo_base::BigBuffer>) override {
    events.push_back(1);
    if (pause_on_response)
      receiver.Pause();
  }
  void OnReceiveRedirect(const net::RedirectInfo&,
                         network::mojom::URLResponseHeadPtr) override {
    events.push_back(4);
  }
  void OnUploadProgress(int64_t, int64_t, OnUploadProgressCallback reply) override {
    std::move(reply).Run();
  }
  void OnTransferSizeUpdated(int32_t difference) override {
    events.push_back(10 + difference);
  }
  void OnComplete(const network::URLLoaderCompletionStatus&) override {
    events.push_back(3);
  }
  std::vector<int> events;
  bool pause_on_response = true;
  mojo::Receiver<network::mojom::URLLoaderClient> receiver{this};
};

void TypedPauseResumeAndLifetime() {
  URLClientProbe original;
  mojo::Remote<network::mojom::URLLoaderClient> remote;
  original.receiver.Bind(remote.BindNewPipeAndPassReceiver());
  remote->OnReceiveResponse(network::mojom::URLResponseHead::New(), {}, std::nullopt);
  remote->OnComplete(network::URLLoaderCompletionStatus());
  Require(original.events.empty(), "typed messages are actually queued");
  base::RunLoop().RunUntilIdle();
  Require(original.events == std::vector<int>{1}, "Pause retains queued completion");
  original.receiver.Resume();
  original.events.push_back(2);  // ThrottlingURLLoader forwards the response here.
  Require(original.events == std::vector<int>({1, 2}), "Resume never dispatches inline");
  base::RunLoop().RunUntilIdle();
  Require(original.events == std::vector<int>({1, 2, 3}),
          "resumed completion preserves response-forwarding order");

  remote->OnTransferSizeUpdated(6);
  remote.FlushForTesting();
  Require(original.events.back() == 16, "remote synchronous flush crosses queue barrier");
  bool flushed = false;
  remote->OnTransferSizeUpdated(5);
  remote.FlushAsyncForTesting(base::BindOnce([](bool* complete) { *complete = true; }, &flushed));
  Require(!flushed, "async flush does not pretend immediate completion");
  base::RunLoop().RunUntilIdle();
  Require(flushed && original.events.back() == 15, "async flush replies after prior typed call");
  remote->OnTransferSizeUpdated(7);
  original.receiver.FlushForTesting();
  Require(original.events.back() == 17, "FlushForTesting crosses a real queue barrier");
  remote->OnTransferSizeUpdated(8);
  auto pending = original.receiver.Unbind();
  base::RunLoop().RunUntilIdle();
  Require(original.events.back() == 17, "Unbind prevents old posted tasks reaching old implementation");
  URLClientProbe rebound;
  rebound.receiver.Bind(std::move(pending));
  base::RunLoop().RunUntilIdle();
  Require(rebound.events == std::vector<int>{18}, "rebind dispatches retained message to new implementation");
  remote->OnTransferSizeUpdated(9);
  rebound.receiver.reset();
  base::RunLoop().RunUntilIdle();
  Require(rebound.events == std::vector<int>{18}, "closed receiver discards queued calls safely");
}

class UploadSequenceProbe {
 public:
  void CheckSequence() const {
    Require(runner_->RunsTasksInCurrentSequence(),
            "upload getter executes on its receiver sequence");
  }
  void Write(mojo::ScopedDataPipeProducerHandle pipe,
             base::OnceCallback<void(int32_t, uint64_t)> reply) {
    CheckSequence();
    ++writes;
    uint32_t size = 3;
    const MojoResult result = pipe->WriteData("abc", &size, MOJO_WRITE_DATA_FLAG_NONE);
    Require(result == MOJO_RESULT_OK || result == MOJO_RESULT_FAILED_PRECONDITION,
            "upload writes bytes or observes a cancelled consumer");
    std::move(reply).Run(result == MOJO_RESULT_OK ? 0 : -1,
                         result == MOJO_RESULT_OK ? size : 0);
  }
  unsigned writes = 0;

 private:
  const scoped_refptr<base::SequencedTaskRunner> runner_ =
      base::SequencedTaskRunner::GetCurrentDefault();
};

template <typename Interface>
class DataGetterProbe final : public Interface, public UploadSequenceProbe {
 public:
  explicit DataGetterProbe(mojo::PendingReceiver<Interface> receiver) {
    receivers_.Add(this, std::move(receiver));
  }
  void Read(mojo::ScopedDataPipeProducerHandle pipe,
            typename Interface::ReadCallback callback) override {
    Write(std::move(pipe), std::move(callback));
  }
  void Clone(mojo::PendingReceiver<Interface> receiver) override {
    CheckSequence();
    receivers_.Add(this, std::move(receiver));
  }
  void Close() { receivers_.Clear(); }

 private:
  mojo::ReceiverSet<Interface> receivers_;
};

template <typename Interface>
class ChunkedGetterProbe final : public Interface, public UploadSequenceProbe {
 public:
  explicit ChunkedGetterProbe(mojo::PendingReceiver<Interface> receiver)
      : receiver_(this, std::move(receiver)) {}
  void GetSize(typename Interface::GetSizeCallback callback) override {
    CheckSequence();
    Require(!reply_, "chunked GetSize runs once before StartReading");
    reply_ = std::move(callback);
  }
  void StartReading(mojo::ScopedDataPipeProducerHandle pipe) override {
    CheckSequence();
    Require(!!reply_, "queued chunked upload preserves GetSize/StartReading order");
    Write(std::move(pipe), std::move(reply_));
  }
  void Close() { receiver_.reset(); }

 private:
  typename Interface::GetSizeCallback reply_;
  mojo::Receiver<Interface> receiver_;
};

template <typename Interface, bool chunked, typename ReceiverInterface = Interface>
void CrossThreadUploadGetter(scoped_refptr<base::SequencedTaskRunner> receiver_runner) {
  mojo::Remote<Interface> remote;
  using Probe = std::conditional_t<chunked, ChunkedGetterProbe<ReceiverInterface>,
                                  DataGetterProbe<ReceiverInterface>>;
  auto pending = remote.BindNewPipeAndPassReceiver();
  base::SequenceBound<Probe> receiver(receiver_runner,
      mojo::PendingReceiver<ReceiverInterface>(pending.PassPipe()));
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  Result(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK,
         "create cross-sequence upload pipe");
  const auto caller = base::SequencedTaskRunner::GetCurrentDefault();
  base::RunLoop complete;
  auto reply = base::BindOnce(
      [](scoped_refptr<base::SequencedTaskRunner> caller,
         base::OnceClosure quit, int32_t status, uint64_t size) {
        Require(caller->RunsTasksInCurrentSequence(),
                "upload response returns to the requester sequence");
        Require(status == 0 && size == 3, "upload reports bytes actually produced");
        std::move(quit).Run();
      }, caller, complete.QuitClosure());
  if constexpr (chunked) {
    remote->GetSize(std::move(reply));
    remote->StartReading(std::move(producer));
  } else {
    remote->Read(std::move(producer), std::move(reply));
  }
  complete.Run();
  char bytes[4] = {};
  uint32_t size = sizeof(bytes);
  Result(consumer->ReadData(bytes, &size, MOJO_READ_DATA_FLAG_NONE), MOJO_RESULT_OK,
         "requester receives real cross-sequence upload bytes");
  Require(size == 3 && std::memcmp(bytes, "abc", 3) == 0,
          "queued upload owns the producer until receiver dispatch");
  receiver.SynchronouslyResetForTest();
}

template <bool chunked>
void UploadStreamLifetime() {
  using Interface = std::conditional_t<chunked,
      network::mojom::ChunkedDataPipeGetter, network::mojom::DataPipeGetter>;
  using ReceiverInterface = std::conditional_t<chunked,
      network::mojom::blink::ChunkedDataPipeGetter, Interface>;
  using Probe = std::conditional_t<chunked, ChunkedGetterProbe<ReceiverInterface>,
                                  DataGetterProbe<ReceiverInterface>>;
  const auto caller = base::SequencedTaskRunner::GetCurrentDefault();
  for (int close_mode = 0; close_mode < 3; ++close_mode) {
    mojo::PendingRemote<Interface> pending;
    auto receiver = pending.InitWithNewPipeAndPassReceiver();
    std::unique_ptr<Probe> probe;
    if (close_mode == 0)
      receiver.reset(); // Receiver dies before curl binds its pending remote.
    else
      probe = std::make_unique<Probe>(mojo::PendingReceiver<ReceiverInterface>(receiver.PassPipe()));
    auto element = std::make_unique<mini_electron::FlattenHTTPBodyElement>();
    if constexpr (chunked) {
      element->type = mini_electron::FlattenHTTPBodyElement::TypeChunkedDataPipe;
      element->pendingChunkedGetter = std::move(pending);
    } else {
      element->type = mini_electron::FlattenHTTPBodyElement::TypeDataPipe;
      element->pendingGetter = std::move(pending);
    }
    std::vector<std::unique_ptr<mini_electron::FlattenHTTPBodyElement>> elements;
    elements.push_back(std::move(element));
    unsigned resumed = 0;
    auto stream = std::make_unique<mini_electron::FlattenHTTPBodyElementStream>(
        std::move(elements), false, base::BindRepeating(
            [](scoped_refptr<base::SequencedTaskRunner> caller, unsigned* resumed) {
              Require(caller->RunsTasksInCurrentSequence(),
                      "curl resume stays on the stream sequence");
              ++*resumed;
            }, caller, &resumed));
    char byte = 0;
    Require(stream->read(&byte, 1, 1) == CURL_READFUNC_PAUSE,
            "upload pauses rather than invoking a getter inline");
    if (close_mode == 1)
      probe->Close(); // Discard queued calls and their owned producer.
    else if (close_mode == 2)
      stream.reset(); // A dispatched late reply must not reach the dead stream.
    base::RunLoop().RunUntilIdle();
    if (close_mode == 2) {
      Require(probe->writes == 1 && resumed == 0,
              "cancelled stream drops late replies while draining sent calls");
    } else {
      Require(resumed > 0 && stream->read(&byte, 1, 1) == CURL_READFUNC_ABORT,
              "closed upload receiver wakes curl and terminates instead of hanging");
      if (probe)
        Require(probe->writes == 0, "closed receiver cannot invoke the old uploader");
    }
  }
}

void UploadGetterSequenceAndLifetime() {
  base::Thread receiver_thread("Blink upload receiver");
  Require(receiver_thread.Start(), "start upload receiver sequence");
  CrossThreadUploadGetter<network::mojom::DataPipeGetter, false>(receiver_thread.task_runner());
  CrossThreadUploadGetter<network::mojom::blink::DataPipeGetter, false>(receiver_thread.task_runner());
  CrossThreadUploadGetter<network::mojom::ChunkedDataPipeGetter, true,
      network::mojom::blink::ChunkedDataPipeGetter>(receiver_thread.task_runner());
  CrossThreadUploadGetter<network::mojom::blink::ChunkedDataPipeGetter, true>(receiver_thread.task_runner());
  UploadStreamLifetime<false>();
  UploadStreamLifetime<true>();
}

template <typename Interface> class BlobClientProbe final : public Interface {
 public:
  void OnCalculatedSize(uint64_t total, uint64_t expected) override {
    Require(total == expected, "queued calculated-size arguments preserved");
    ++calculated;
  }
  void OnComplete(int32_t status, uint64_t length) override {
    Require(status == 0 && length == 10, "queued completion arguments preserved");
    ++completed;
  }
  unsigned calculated = 0;
  unsigned completed = 0;
  mojo::Receiver<Interface> receiver{this};
};

template <typename Interface> void TypedBlockingReceive() {
  BlobClientProbe<Interface> client;
  mojo::Remote<Interface> remote;
  auto pending = remote.BindNewPipeAndPassReceiver();
  remote->OnCalculatedSize(10, 10);
  Require(client.calculated == 0, "pending receiver does not run callbacks");
  client.receiver.Bind(std::move(pending));
  Require(client.receiver.WaitForIncomingCall() && client.calculated == 1,
          "WaitForIncomingCall actually dispatches calculated-size");
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([](mojo::Remote<Interface>* sender) {
        (*sender)->OnComplete(0, 10);
      }, &remote));
  Require(client.receiver.WaitForIncomingCall() && client.completed == 1,
          "blocking receive pumps a later producer and dispatches completion");
  remote->OnCalculatedSize(20, 20);
  remote.reset();
  Require(client.receiver.WaitForIncomingCall() && client.calculated == 2,
          "messages sent before peer close are dispatched before disconnect");
  Require(!client.receiver.WaitForIncomingCall(), "disconnected empty receiver returns false");
  client.receiver.reset();
  Require(!client.receiver.WaitForIncomingCall(), "unbound receiver returns false");

  BlobClientProbe<Interface> disconnect_client;
  mojo::Remote<Interface> disconnect_remote;
  disconnect_client.receiver.Bind(disconnect_remote.BindNewPipeAndPassReceiver());
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([](mojo::Remote<Interface>* sender) {
        sender->reset();
      }, &disconnect_remote));
  Require(!disconnect_client.receiver.WaitForIncomingCall(),
          "peer close wakes a genuinely waiting receiver with false");
}

void UrgentScopeAndStackStorage() {
  Require(!mojo::UrgentMessageScope::IsInUrgentScope(), "urgent state starts false");
  {
    mojo::UrgentMessageScope outer;
    Require(mojo::UrgentMessageScope::IsInUrgentScope(), "urgent scope sets actual TLS state");
    {
      mojo::UrgentMessageScope inner;
      mojo::UrgentMessageScope moved(std::move(inner));
      Require(mojo::UrgentMessageScope::IsInUrgentScope(), "moved nested scope retains urgency");
    }
    Require(mojo::UrgentMessageScope::IsInUrgentScope(), "nested destruction restores outer urgency");
    bool isolated = false;
    std::thread other([](bool* isolated) {
      Require(!mojo::UrgentMessageScope::IsInUrgentScope(), "urgent state does not leak between threads");
      {
        mojo::UrgentMessageScope scope;
        Require(mojo::UrgentMessageScope::IsInUrgentScope(), "worker owns independent urgent state");
      }
      *isolated = !mojo::UrgentMessageScope::IsInUrgentScope();
    }, &isolated);
    other.join();
    Require(isolated && mojo::UrgentMessageScope::IsInUrgentScope(), "worker restores its state without resetting parent");
    mojo::Message urgent(99, mojo::Message::kFlagIsUrgent, 0);
    Require(urgent.has_flag(mojo::Message::kFlagIsUrgent), "three-argument generated constructor preserves urgent flag");
  }
  Require(!mojo::UrgentMessageScope::IsInUrgentScope(), "urgent scope restores initial state");

  base::StackVector<int, 4> values;
  const uintptr_t object_begin = reinterpret_cast<uintptr_t>(&values);
  const uintptr_t object_end = object_begin + sizeof(values);
  const uintptr_t inline_data = reinterpret_cast<uintptr_t>(values->data());
  Require(inline_data >= object_begin && inline_data < object_end, "small vector uses actual object-local stack storage");
  for (int value = 0; value < 4; ++value)
    values->push_back(value);
  Require(reinterpret_cast<uintptr_t>(values->data()) == inline_data, "four entries require no fallback allocation");
  base::StackVector<int, 4> copy(values);
  Require(copy->data() != values->data() && copy[3] == 3, "stack-vector copy owns separate stack storage");
  values->push_back(4);
  Require(reinterpret_cast<uintptr_t>(values->data()) != inline_data && values[0] == 0 && values[4] == 4,
          "overflow uses real fallback allocation while preserving elements");
}

class DrainerProbe final : public mojo::DataPipeDrainer::Client {
 public:
  void OnDataAvailable(base::span<const uint8_t> data) override {
    bytes.insert(bytes.end(), data.begin(), data.end());
    if (destroy_on_data)
      drainer.reset();
  }
  void OnDataComplete() override { completed = true; }
  std::vector<uint8_t> bytes;
  bool completed = false;
  bool destroy_on_data = false;
  std::unique_ptr<mojo::DataPipeDrainer> drainer;
};

void SpanDrainerLifetime() {
  for (bool destroy_on_data : {false, true}) {
    MojoHandle producer = 0, consumer = 0;
    Result(MojoCreateDataPipe(nullptr, &producer, &consumer), MOJO_RESULT_OK, "create real drainer pipe");
    DrainerProbe probe;
    probe.destroy_on_data = destroy_on_data;
    probe.drainer = std::make_unique<mojo::DataPipeDrainer>(
        &probe, mojo::ScopedDataPipeConsumerHandle(mojo::DataPipeConsumerHandle(consumer)));
    uint32_t size = 3;
    Result(MojoWriteData(producer, "abc", &size, nullptr), MOJO_RESULT_OK, "write drainer bytes");
    Result(MojoClose(producer), MOJO_RESULT_OK, "close drainer producer");
    base::RunLoop().RunUntilIdle();
    Require(probe.bytes == std::vector<uint8_t>({'a', 'b', 'c'}), "span drainer returns actual bytes");
    if (destroy_on_data)
      Require(!probe.drainer, "client can destroy drainer inside span callback");
    else
      Require(probe.completed, "span drainer signals actual peer-close completion");
  }
}

void CrossProcessWireSchema() {
  const std::vector<char> payload{'m', 'o', 'j', 'o'};
  const MojoLiteMessagePipeHandles handles(1, {12, 13});
  CrossProcessConnector_PostMessage message(101, 202, 1, payload, handles);
  CrossProcessConnector_PostMessage::Param decoded;
  Require(CrossProcessConnector_PostMessage::Read(&message, &decoded),
          "actual IPC control schema deserializes message-pipe capability tag");
  Require(decoded.a == 101 && decoded.b == 202 && decoded.c == 1 &&
              decoded.d == payload && decoded.e == handles,
          "actual IPC wire preserves kind and handle IDs as distinct fields");
  IPC::Message truncated;
  IPC::WriteParam(&truncated, static_cast<uintptr_t>(101));
  IPC::WriteParam(&truncated, static_cast<uintptr_t>(202));
  IPC::WriteParam(&truncated, 1);
  IPC::WriteParam(&truncated, payload);
  Require(!CrossProcessConnector_PostMessage::Read(&truncated, &decoded),
          "missing capability aggregate is rejected by actual IPC decoder");
}

void SameSequenceSignalWait() {
  MojoHandle producer = 0, consumer = 0;
  Result(MojoCreateDataPipe(nullptr, &producer, &consumer), MOJO_RESULT_OK,
         "create same-sequence producer pipe");
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([](MojoHandle producer) {
        uint32_t size = 1;
        Result(MojoWriteData(producer, "q", &size, nullptr), MOJO_RESULT_OK,
               "same-sequence producer makes progress during Wait");
      }, producer));
  Result(mojo::Wait(mojo::Handle(consumer), MOJO_HANDLE_SIGNAL_READABLE), MOJO_RESULT_OK,
         "signal wait pumps real same-sequence data producer");
  char byte = 0;
  uint32_t size = 1;
  Result(MojoReadData(consumer, nullptr, &byte, &size), MOJO_RESULT_OK,
         "read same-sequence producer result");
  Require(size == 1 && byte == 'q', "signal wait retained producer bytes");
  Result(MojoClose(producer), MOJO_RESULT_OK, "close same-sequence producer");
  Result(MojoClose(consumer), MOJO_RESULT_OK, "close same-sequence consumer");
}
}  // namespace

int main() {
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::DEFAULT);
  Result(MojoInitialize(nullptr), MOJO_RESULT_OK, "initialize actual lite transport");
  DataPipesAndTraps();
  SharedBufferCapability();
  CppMessageHandoff();
  TypedPauseResumeAndLifetime();
  UploadGetterSequenceAndLifetime();
  TypedBlockingReceive<blink::mojom::BlobReaderClient>();
  TypedBlockingReceive<blink::mojom::blink::BlobReaderClient>();
  SameSequenceSignalWait();
  CrossProcessWireSchema();
  UrgentScopeAndStackStorage();
  SpanDrainerLifetime();
  base::RunLoop().RunUntilIdle();
  std::puts("PASS: Mojo ownership, data pipes/traps, typed lifetime and upload sequence safety");
  return 0;
}
