// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/core.h"

#include <string.h>

#include <algorithm>
#include <utility>

#include "base/bind.h"
#include "base/containers/stack_container.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/memory/writable_shared_memory_region.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/trace_event/memory_dump_manager.h"
#include "build/build_config.h"
#include "mojo/edk/embedder/platform_handle_utils.h"
#include "mojo/edk/embedder/process_error_callback.h"
#include "mojo/edk/system/channel.h"
#include "mojo/edk/system/configuration.h"
#include "mojo/edk/system/data_pipe_consumer_dispatcher.h"
#include "mojo/edk/system/data_pipe_producer_dispatcher.h"
#include "mojo/edk/system/handle_signals_state.h"
#include "mojo/edk/system/invitation_dispatcher.h"
#include "mojo/edk/system/message_pipe_dispatcher.h"
#include "mojo/edk/system/platform_handle_dispatcher.h"
#include "mojo/edk/system/platform_shared_memory_mapping.h"
#include "mojo/edk/system/ports/event.h"
#include "mojo/edk/system/ports/name.h"
#include "mojo/edk/system/ports/node.h"
#include "mojo/edk/system/request_context.h"
#include "mojo/edk/system/scoped_process_handle.h"
#include "mojo/edk/system/shared_buffer_dispatcher.h"
#include "mojo/edk/system/user_message_impl.h"
#include "mojo/edk/system/watcher_dispatcher.h"

namespace mojo {
namespace edk {

namespace {

// This is an unnecessarily large limit that is relatively easy to enforce.
const uint32_t kMaxHandlesPerMessage = 1024 * 1024;

// TODO(rockot): Maybe we could negotiate a debugging pipe ID for cross-process
// pipes too; for now we just use a constant. This only affects bootstrap pipes.
const uint64_t kUnknownPipeIdForDebug = 0x7f7f7f7f7f7f7f7fUL;

void RunMojoProcessErrorHandler(scoped_refptr<base::TaskRunner> task_runner,
                                MojoProcessErrorHandler handler,
                                uintptr_t context,
                                const std::string& error) {
  // Run the handler asynchronously to ensure no Mojo core reentrancy.
  task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](MojoProcessErrorHandler handler, uintptr_t context,
             const std::string& error) {
            MojoProcessErrorDetails details;
            details.struct_size = sizeof(details);
            DCHECK(base::IsValueInRangeForNumericType<uint32_t>(error.size()));
            details.error_message_length = static_cast<uint32_t>(error.size());
            details.error_message = error.data();
            handler(context, &details);
          },
          handler, context, error));
}

}  // namespace

Core::Core() {
  handles_.reset(new HandleTable);
  base::trace_event::MemoryDumpManager::GetInstance()->RegisterDumpProvider(
      handles_.get(), "MojoHandleTable", nullptr);
}

Core::~Core() {
  if (node_controller_ && node_controller_->io_task_runner()) {
    // If this races with IO thread shutdown the callback will be dropped and
    // the NodeController will be shutdown on this thread anyway, which is also
    // just fine.
    scoped_refptr<base::TaskRunner> io_task_runner =
        node_controller_->io_task_runner();
    io_task_runner->PostTask(FROM_HERE,
                             base::Bind(&Core::PassNodeControllerToIOThread,
                                        base::Passed(&node_controller_)));
  }
  base::trace_event::MemoryDumpManager::GetInstance()
      ->UnregisterAndDeleteDumpProviderSoon(std::move(handles_));
}

void Core::SetIOTaskRunner(scoped_refptr<base::TaskRunner> io_task_runner) {
  GetNodeController()->SetIOTaskRunner(io_task_runner);
}

NodeController* Core::GetNodeController() {
  base::AutoLock lock(node_controller_lock_);
  if (!node_controller_)
    node_controller_.reset(new NodeController(this));
  return node_controller_.get();
}

scoped_refptr<Dispatcher> Core::GetDispatcher(MojoHandle handle) {
  base::AutoLock lock(handles_->GetLock());
  return handles_->GetDispatcher(handle);
}

scoped_refptr<Dispatcher> Core::GetAndRemoveDispatcher(MojoHandle handle) {
  scoped_refptr<Dispatcher> dispatcher;
  base::AutoLock lock(handles_->GetLock());
  handles_->GetAndRemoveDispatcher(handle, &dispatcher);
  return dispatcher;
}

void Core::SetDefaultProcessErrorCallback(
    const ProcessErrorCallback& callback) {
  default_process_error_callback_ = callback;
}

MojoHandle Core::CreatePartialMessagePipe(ports::PortRef* peer) {
  RequestContext request_context;
  ports::PortRef local_port;
  GetNodeController()->node()->CreatePortPair(&local_port, peer);
  return AddDispatcher(new MessagePipeDispatcher(
      GetNodeController(), local_port, kUnknownPipeIdForDebug, 0));
}

MojoHandle Core::CreatePartialMessagePipe(const ports::PortRef& port) {
  RequestContext request_context;
  return AddDispatcher(new MessagePipeDispatcher(GetNodeController(), port,
                                                 kUnknownPipeIdForDebug, 1));
}

void Core::SendBrokerClientInvitation(
    base::ProcessHandle target_process,
    ConnectionParams connection_params,
    const std::vector<std::pair<std::string, ports::PortRef>>& attached_ports,
    const ProcessErrorCallback& process_error_callback) {
  RequestContext request_context;
  GetNodeController()->SendBrokerClientInvitation(
      target_process, std::move(connection_params), attached_ports,
      process_error_callback);
}

void Core::AcceptBrokerClientInvitation(ConnectionParams connection_params) {
  RequestContext request_context;
  GetNodeController()->AcceptBrokerClientInvitation(
      std::move(connection_params));
}

uint64_t Core::ConnectToPeer(ConnectionParams connection_params,
                             const ports::PortRef& port) {
  RequestContext request_context;
  return GetNodeController()->ConnectToPeer(std::move(connection_params), port);
}

void Core::ClosePeerConnection(uint64_t peer_connection_id) {
  RequestContext request_context;
  GetNodeController()->ClosePeerConnection(peer_connection_id);
}

void Core::SetMachPortProvider(base::PortProvider* port_provider) {
#if defined(OS_MACOSX) && !defined(OS_IOS)
  GetNodeController()->CreateMachPortRelay(port_provider);
#endif
}

MojoHandle Core::AddDispatcher(scoped_refptr<Dispatcher> dispatcher) {
  base::AutoLock lock(handles_->GetLock());
  return handles_->AddDispatcher(dispatcher);
}

bool Core::AddDispatchersFromTransit(
    const std::vector<Dispatcher::DispatcherInTransit>& dispatchers,
    MojoHandle* handles) {
  bool failed = false;
  {
    base::AutoLock lock(handles_->GetLock());
    if (!handles_->AddDispatchersFromTransit(dispatchers, handles))
      failed = true;
  }
  if (failed) {
    for (auto d : dispatchers) {
      if (d.dispatcher)
        d.dispatcher->Close();
    }
    return false;
  }
  return true;
}

MojoResult Core::AcquireDispatchersForTransit(
    const MojoHandle* handles,
    size_t num_handles,
    std::vector<Dispatcher::DispatcherInTransit>* dispatchers) {
  base::AutoLock lock(handles_->GetLock());
  MojoResult rv = handles_->BeginTransit(handles, num_handles, dispatchers);
  if (rv != MOJO_RESULT_OK)
    handles_->CancelTransit(*dispatchers);
  return rv;
}

void Core::ReleaseDispatchersForTransit(
    const std::vector<Dispatcher::DispatcherInTransit>& dispatchers,
    bool in_transit) {
  base::AutoLock lock(handles_->GetLock());
  if (in_transit)
    handles_->CompleteTransitAndClose(dispatchers);
  else
    handles_->CancelTransit(dispatchers);
}

MojoResult Core::CreatePlatformHandleWrapper(
    ScopedPlatformHandle platform_handle,
    MojoHandle* wrapper_handle) {
  MojoHandle h = AddDispatcher(
      PlatformHandleDispatcher::Create(std::move(platform_handle)));
  if (h == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  *wrapper_handle = h;
  return MOJO_RESULT_OK;
}

MojoResult Core::PassWrappedPlatformHandle(
    MojoHandle wrapper_handle,
    ScopedPlatformHandle* platform_handle) {
  base::AutoLock lock(handles_->GetLock());
  scoped_refptr<Dispatcher> d;
  MojoResult result = handles_->GetAndRemoveDispatcher(wrapper_handle, &d);
  if (result != MOJO_RESULT_OK)
    return result;
  if (d->GetType() == Dispatcher::Type::PLATFORM_HANDLE) {
    PlatformHandleDispatcher* phd =
        static_cast<PlatformHandleDispatcher*>(d.get());
    *platform_handle = phd->PassPlatformHandle();
  } else {
    result = MOJO_RESULT_INVALID_ARGUMENT;
  }
  d->Close();
  return result;
}

void Core::RequestShutdown(const base::Closure& callback) {
  GetNodeController()->RequestShutdown(callback);
}

MojoHandle Core::ExtractMessagePipeFromInvitation(const std::string& name) {
  RequestContext request_context;
  ports::PortRef port0, port1;
  GetNodeController()->node()->CreatePortPair(&port0, &port1);
  MojoHandle handle = AddDispatcher(new MessagePipeDispatcher(
      GetNodeController(), port0, kUnknownPipeIdForDebug, 1));
  GetNodeController()->MergePortIntoInviter(name, port1);
  return handle;
}

MojoTimeTicks Core::GetTimeTicksNow() {
  return base::TimeTicks::Now().ToInternalValue();
}

MojoResult Core::Close(MojoHandle handle) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher;
  {
    base::AutoLock lock(handles_->GetLock());
    MojoResult rv = handles_->GetAndRemoveDispatcher(handle, &dispatcher);
    if (rv != MOJO_RESULT_OK)
      return rv;
  }
  dispatcher->Close();
  return MOJO_RESULT_OK;
}

MojoResult Core::QueryHandleSignalsState(
    MojoHandle handle,
    MojoHandleSignalsState* signals_state) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher = GetDispatcher(handle);
  if (!dispatcher || !signals_state)
    return MOJO_RESULT_INVALID_ARGUMENT;
  *signals_state = dispatcher->GetHandleSignalsState();
  return MOJO_RESULT_OK;
}

MojoResult Core::CreateTrap(MojoTrapEventHandler handler,
                            const MojoCreateTrapOptions* options,
                            MojoHandle* trap_handle) {
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;
  if (!trap_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  *trap_handle = AddDispatcher(new WatcherDispatcher(handler));
  if (*trap_handle == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  return MOJO_RESULT_OK;
}

MojoResult Core::AddTrigger(MojoHandle trap_handle,
                            MojoHandle handle,
                            MojoHandleSignals signals,
                            MojoTriggerCondition condition,
                            uintptr_t context,
                            const MojoAddTriggerOptions* options) {
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;
  scoped_refptr<Dispatcher> watcher = GetDispatcher(trap_handle);
  if (!watcher || watcher->GetType() != Dispatcher::Type::WATCHER)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> dispatcher = GetDispatcher(handle);
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return watcher->WatchDispatcher(std::move(dispatcher), signals, condition,
                                  context);
}

MojoResult Core::RemoveTrigger(MojoHandle trap_handle,
                               uintptr_t context,
                               const MojoRemoveTriggerOptions* options) {
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;
  scoped_refptr<Dispatcher> watcher = GetDispatcher(trap_handle);
  if (!watcher || watcher->GetType() != Dispatcher::Type::WATCHER)
    return MOJO_RESULT_INVALID_ARGUMENT;
  return watcher->CancelWatch(context);
}

MojoResult Core::ArmTrap(MojoHandle trap_handle,
                         const MojoArmTrapOptions* options,
                         uint32_t* num_ready_triggers,
                         uintptr_t* ready_triggers,
                         MojoResult* ready_results,
                         MojoHandleSignalsState* ready_signals_states) {
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;
  scoped_refptr<Dispatcher> watcher = GetDispatcher(trap_handle);
  if (!watcher || watcher->GetType() != Dispatcher::Type::WATCHER)
    return MOJO_RESULT_INVALID_ARGUMENT;
  return watcher->Arm(num_ready_triggers, ready_triggers, ready_results,
                      ready_signals_states);
}

MojoResult Core::CreateMessage(const MojoCreateMessageOptions* options,
                               MojoMessageHandle* message_handle) {
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;
  *message_handle = reinterpret_cast<MojoMessageHandle>(
      UserMessageImpl::CreateEventForNewMessage().release());
  return MOJO_RESULT_OK;
}

MojoResult Core::DestroyMessage(MojoMessageHandle message_handle) {
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;
  delete reinterpret_cast<ports::UserMessageEvent*>(message_handle);
  return MOJO_RESULT_OK;
}

MojoResult Core::SerializeMessage(MojoMessageHandle message_handle,
                                  const MojoSerializeMessageOptions* options) {
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;
  RequestContext request_context;
  return reinterpret_cast<ports::UserMessageEvent*>(message_handle)
      ->GetMessage<UserMessageImpl>()
      ->SerializeIfNecessary();
}

MojoResult Core::AppendMessageData(MojoMessageHandle message_handle,
                                   uint32_t additional_payload_size,
                                   const MojoHandle* handles,
                                   uint32_t num_handles,
                                   const MojoAppendMessageDataOptions* options,
                                   void** buffer,
                                   uint32_t* buffer_size) {
  if (!message_handle || (num_handles && !handles))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;
  auto* message = reinterpret_cast<ports::UserMessageEvent*>(message_handle)
                      ->GetMessage<UserMessageImpl>();
  MojoResult rv =
      message->AppendData(additional_payload_size, handles, num_handles);
  if (rv != MOJO_RESULT_OK)
    return rv;

  if (options && (options->flags & MOJO_APPEND_MESSAGE_DATA_FLAG_COMMIT_SIZE)) {
    RequestContext request_context;
    message->CommitSize();
  }

  if (buffer)
    *buffer = message->user_payload();
  if (buffer_size) {
    *buffer_size =
        base::checked_cast<uint32_t>(message->user_payload_capacity());
  }
  return MOJO_RESULT_OK;
}

MojoResult Core::GetMessageData(MojoMessageHandle message_handle,
                                const MojoGetMessageDataOptions* options,
                                void** buffer,
                                uint32_t* num_bytes,
                                MojoHandle* handles,
                                uint32_t* num_handles) {
  if (!message_handle || (num_handles && *num_handles && !handles))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  auto* message = reinterpret_cast<ports::UserMessageEvent*>(message_handle)
                      ->GetMessage<UserMessageImpl>();
  if (!message->IsSerialized() || !message->IsTransmittable())
    return MOJO_RESULT_FAILED_PRECONDITION;

  if (num_bytes) {
    base::CheckedNumeric<uint32_t> payload_size = message->user_payload_size();
    *num_bytes = payload_size.ValueOrDie();
  }

  if (message->user_payload_size() > 0) {
    if (!num_bytes || !buffer)
      return MOJO_RESULT_RESOURCE_EXHAUSTED;

    *buffer = message->user_payload();
  } else if (buffer) {
    *buffer = nullptr;
  }

  if (options && (options->flags & MOJO_GET_MESSAGE_DATA_FLAG_IGNORE_HANDLES))
    return MOJO_RESULT_OK;

  uint32_t max_num_handles = 0;
  if (num_handles) {
    max_num_handles = *num_handles;
    *num_handles = static_cast<uint32_t>(message->num_handles());
  }

  if (message->num_handles() > max_num_handles ||
      message->num_handles() > kMaxHandlesPerMessage) {
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  RequestContext request_context;
  return message->ExtractSerializedHandles(
      UserMessageImpl::ExtractBadHandlePolicy::kAbort, handles);
}

MojoResult Core::SetMessageContext(
    MojoMessageHandle message_handle,
    uintptr_t context,
    MojoMessageContextSerializer serializer,
    MojoMessageContextDestructor destructor,
    const MojoSetMessageContextOptions* options) {
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;
  auto* message = reinterpret_cast<ports::UserMessageEvent*>(message_handle)
                      ->GetMessage<UserMessageImpl>();
  return message->SetContext(context, serializer, destructor);
}

MojoResult Core::GetMessageContext(MojoMessageHandle message_handle,
                                   const MojoGetMessageContextOptions* options,
                                   uintptr_t* context) {
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options && options->struct_size != sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  auto* message = reinterpret_cast<ports::UserMessageEvent*>(message_handle)
                      ->GetMessage<UserMessageImpl>();
  if (!message->HasContext())
    return MOJO_RESULT_NOT_FOUND;

  *context = message->context();
  return MOJO_RESULT_OK;
}

MojoResult Core::CreateMessagePipe(const MojoCreateMessagePipeOptions* options,
                                   MojoHandle* message_pipe_handle0,
                                   MojoHandle* message_pipe_handle1) {
  RequestContext request_context;
  ports::PortRef port0, port1;
  GetNodeController()->node()->CreatePortPair(&port0, &port1);

  DCHECK(message_pipe_handle0);
  DCHECK(message_pipe_handle1);

  uint64_t pipe_id = base::RandUint64();

  *message_pipe_handle0 = AddDispatcher(
      new MessagePipeDispatcher(GetNodeController(), port0, pipe_id, 0));
  if (*message_pipe_handle0 == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  *message_pipe_handle1 = AddDispatcher(
      new MessagePipeDispatcher(GetNodeController(), port1, pipe_id, 1));
  if (*message_pipe_handle1 == MOJO_HANDLE_INVALID) {
    scoped_refptr<Dispatcher> dispatcher0;
    {
      base::AutoLock lock(handles_->GetLock());
      handles_->GetAndRemoveDispatcher(*message_pipe_handle0, &dispatcher0);
    }
    dispatcher0->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::WriteMessage(MojoHandle message_pipe_handle,
                              MojoMessageHandle message_handle,
                              const MojoWriteMessageOptions* options) {
  RequestContext request_context;
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  auto message_event = base::WrapUnique(
      reinterpret_cast<ports::UserMessageEvent*>(message_handle));
  auto* message = message_event->GetMessage<UserMessageImpl>();
  if (!message || !message->IsTransmittable())
    return MOJO_RESULT_INVALID_ARGUMENT;
  auto dispatcher = GetDispatcher(message_pipe_handle);
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;
  return dispatcher->WriteMessage(std::move(message_event));
}

MojoResult Core::ReadMessage(MojoHandle message_pipe_handle,
                             const MojoReadMessageOptions* options,
                             MojoMessageHandle* message_handle) {
  RequestContext request_context;
  auto dispatcher = GetDispatcher(message_pipe_handle);
  if (!dispatcher || !message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  std::unique_ptr<ports::UserMessageEvent> message_event;
  MojoResult rv = dispatcher->ReadMessage(&message_event);
  if (rv != MOJO_RESULT_OK)
    return rv;

  *message_handle =
      reinterpret_cast<MojoMessageHandle>(message_event.release());
  return MOJO_RESULT_OK;
}

MojoResult Core::FuseMessagePipes(MojoHandle handle0,
                                  MojoHandle handle1,
                                  const MojoFuseMessagePipesOptions* options) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher0;
  scoped_refptr<Dispatcher> dispatcher1;

  bool valid_handles = true;
  {
    base::AutoLock lock(handles_->GetLock());
    MojoResult result0 =
        handles_->GetAndRemoveDispatcher(handle0, &dispatcher0);
    MojoResult result1 =
        handles_->GetAndRemoveDispatcher(handle1, &dispatcher1);
    if (result0 != MOJO_RESULT_OK || result1 != MOJO_RESULT_OK ||
        dispatcher0->GetType() != Dispatcher::Type::MESSAGE_PIPE ||
        dispatcher1->GetType() != Dispatcher::Type::MESSAGE_PIPE)
      valid_handles = false;
  }

  if (!valid_handles) {
    if (dispatcher0)
      dispatcher0->Close();
    if (dispatcher1)
      dispatcher1->Close();
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  MessagePipeDispatcher* mpd0 =
      static_cast<MessagePipeDispatcher*>(dispatcher0.get());
  MessagePipeDispatcher* mpd1 =
      static_cast<MessagePipeDispatcher*>(dispatcher1.get());

  if (!mpd0->Fuse(mpd1))
    return MOJO_RESULT_FAILED_PRECONDITION;

  return MOJO_RESULT_OK;
}

MojoResult Core::NotifyBadMessage(MojoMessageHandle message_handle,
                                  const char* error,
                                  size_t error_num_bytes,
                                  const MojoNotifyBadMessageOptions* options) {
  if (!message_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  auto* message_event =
      reinterpret_cast<ports::UserMessageEvent*>(message_handle);
  auto* message = message_event->GetMessage<UserMessageImpl>();
  if (message->source_node() == ports::kInvalidNodeName) {
    DVLOG(1) << "Received invalid message from unknown node.";
    if (!default_process_error_callback_.is_null())
      default_process_error_callback_.Run(std::string(error, error_num_bytes));
    return MOJO_RESULT_OK;
  }

  GetNodeController()->NotifyBadMessageFrom(
      message->source_node(), std::string(error, error_num_bytes));
  return MOJO_RESULT_OK;
}

MojoResult Core::CreateDataPipe(const MojoCreateDataPipeOptions* options,
                                MojoHandle* data_pipe_producer_handle,
                                MojoHandle* data_pipe_consumer_handle) {
  RequestContext request_context;
  if (options && options->struct_size != sizeof(MojoCreateDataPipeOptions))
    return MOJO_RESULT_INVALID_ARGUMENT;

  MojoCreateDataPipeOptions create_options;
  create_options.struct_size = sizeof(MojoCreateDataPipeOptions);
  create_options.flags = options ? options->flags : 0;
  create_options.element_num_bytes = options ? options->element_num_bytes : 1;
  // TODO(rockot): Use Configuration to get default data pipe capacity.
  create_options.capacity_num_bytes = options && options->capacity_num_bytes
                                          ? options->capacity_num_bytes
                                          : 64 * 1024;
  if (!create_options.element_num_bytes || !create_options.capacity_num_bytes ||
      create_options.capacity_num_bytes < create_options.element_num_bytes) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  base::subtle::PlatformSharedMemoryRegion ring_buffer_region =
      base::WritableSharedMemoryRegion::TakeHandleForSerialization(
          GetNodeController()->CreateSharedBuffer(
              create_options.capacity_num_bytes));

  // NOTE: We demote the writable region to an unsafe region so that the
  // producer handle can be transferred freely. There is no compelling reason
  // to restrict access rights of consumers since they are the exclusive
  // consumer of this pipe, and it would be impossible to support such access
  // control on Android anyway.
  auto writable_region_handle = ring_buffer_region.PassPlatformHandle();
#if defined(OS_POSIX) && !defined(OS_ANDROID) && !defined(OS_FUCHSIA) && \
    (!defined(OS_MACOSX) || defined(OS_IOS))
  // This isn't strictly necessary, but it does make the handle configuration
  // consistent with regular UnsafeSharedMemoryRegions.
  writable_region_handle.readonly_fd.reset();
#endif
  base::UnsafeSharedMemoryRegion producer_region =
      base::UnsafeSharedMemoryRegion::Deserialize(
          base::subtle::PlatformSharedMemoryRegion::Take(
              std::move(writable_region_handle),
              base::subtle::PlatformSharedMemoryRegion::Mode::kUnsafe,
              create_options.capacity_num_bytes, ring_buffer_region.GetGUID()));
  if (!producer_region.IsValid())
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  ports::PortRef port0, port1;
  GetNodeController()->node()->CreatePortPair(&port0, &port1);

  DCHECK(data_pipe_producer_handle);
  DCHECK(data_pipe_consumer_handle);

  base::UnsafeSharedMemoryRegion consumer_region = producer_region.Duplicate();
  uint64_t pipe_id = base::RandUint64();
  scoped_refptr<Dispatcher> producer = DataPipeProducerDispatcher::Create(
      GetNodeController(), port0, std::move(producer_region), create_options,
      pipe_id);
  if (!producer)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  scoped_refptr<Dispatcher> consumer = DataPipeConsumerDispatcher::Create(
      GetNodeController(), port1, std::move(consumer_region), create_options,
      pipe_id);
  if (!consumer) {
    producer->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *data_pipe_producer_handle = AddDispatcher(producer);
  *data_pipe_consumer_handle = AddDispatcher(consumer);
  if (*data_pipe_producer_handle == MOJO_HANDLE_INVALID ||
      *data_pipe_consumer_handle == MOJO_HANDLE_INVALID) {
    if (*data_pipe_producer_handle != MOJO_HANDLE_INVALID) {
      scoped_refptr<Dispatcher> unused;
      base::AutoLock lock(handles_->GetLock());
      handles_->GetAndRemoveDispatcher(*data_pipe_producer_handle, &unused);
    }
    producer->Close();
    consumer->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::WriteData(MojoHandle data_pipe_producer_handle,
                           const void* elements,
                           uint32_t* num_bytes,
                           const MojoWriteDataOptions* options) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_producer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  MojoWriteDataOptions validated_options;
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;

    constexpr MojoWriteDataFlags kSupportedFlags =
        MOJO_WRITE_DATA_FLAG_NONE | MOJO_WRITE_DATA_FLAG_ALL_OR_NONE;
    if (options->flags & ~kSupportedFlags)
      return MOJO_RESULT_UNIMPLEMENTED;
    validated_options.flags = options->flags;
  } else {
    validated_options.flags = MOJO_WRITE_DATA_FLAG_NONE;
  }
  return dispatcher->WriteData(elements, num_bytes, validated_options);
}

MojoResult Core::BeginWriteData(MojoHandle data_pipe_producer_handle,
                                const MojoBeginWriteDataOptions* options,
                                void** buffer,
                                uint32_t* buffer_num_bytes) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_producer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (options->flags != MOJO_BEGIN_WRITE_DATA_FLAG_NONE)
      return MOJO_RESULT_UNIMPLEMENTED;
  }
  return dispatcher->BeginWriteData(buffer, buffer_num_bytes);
}

MojoResult Core::EndWriteData(MojoHandle data_pipe_producer_handle,
                              uint32_t num_bytes_written,
                              const MojoEndWriteDataOptions* options) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_producer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (options->flags != MOJO_END_WRITE_DATA_FLAG_NONE)
      return MOJO_RESULT_UNIMPLEMENTED;
  }
  return dispatcher->EndWriteData(num_bytes_written);
}

MojoResult Core::ReadData(MojoHandle data_pipe_consumer_handle,
                          const MojoReadDataOptions* options,
                          void* elements,
                          uint32_t* num_bytes) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_consumer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  MojoReadDataOptions validated_options;
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;

    constexpr MojoReadDataFlags kSupportedFlags =
        MOJO_READ_DATA_FLAG_NONE | MOJO_READ_DATA_FLAG_ALL_OR_NONE |
        MOJO_READ_DATA_FLAG_DISCARD | MOJO_READ_DATA_FLAG_QUERY |
        MOJO_READ_DATA_FLAG_PEEK;
    if (options->flags & ~kSupportedFlags)
      return MOJO_RESULT_UNIMPLEMENTED;
    validated_options.flags = options->flags;
  } else {
    validated_options.flags = MOJO_WRITE_DATA_FLAG_NONE;
  }
  return dispatcher->ReadData(validated_options, elements, num_bytes);
}

MojoResult Core::BeginReadData(MojoHandle data_pipe_consumer_handle,
                               const MojoBeginReadDataOptions* options,
                               const void** buffer,
                               uint32_t* buffer_num_bytes) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_consumer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (options->flags != MOJO_BEGIN_READ_DATA_FLAG_NONE)
      return MOJO_RESULT_UNIMPLEMENTED;
  }
  return dispatcher->BeginReadData(buffer, buffer_num_bytes);
}

MojoResult Core::EndReadData(MojoHandle data_pipe_consumer_handle,
                             uint32_t num_bytes_read,
                             const MojoEndReadDataOptions* options) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(
      GetDispatcher(data_pipe_consumer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (options->flags != MOJO_END_READ_DATA_FLAG_NONE)
      return MOJO_RESULT_UNIMPLEMENTED;
  }
  return dispatcher->EndReadData(num_bytes_read);
}

MojoResult Core::CreateSharedBuffer(
    uint64_t num_bytes,
    const MojoCreateSharedBufferOptions* options,
    MojoHandle* shared_buffer_handle) {
  RequestContext request_context;
  MojoCreateSharedBufferOptions validated_options = {};
  MojoResult result = SharedBufferDispatcher::ValidateCreateOptions(
      options, &validated_options);
  if (result != MOJO_RESULT_OK)
    return result;

  scoped_refptr<SharedBufferDispatcher> dispatcher;
  result = SharedBufferDispatcher::Create(
      validated_options, GetNodeController(), num_bytes, &dispatcher);
  if (result != MOJO_RESULT_OK) {
    DCHECK(!dispatcher);
    return result;
  }

  *shared_buffer_handle = AddDispatcher(dispatcher);
  if (*shared_buffer_handle == MOJO_HANDLE_INVALID) {
    LOG(ERROR) << "Handle table full";
    dispatcher->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::DuplicateBufferHandle(
    MojoHandle buffer_handle,
    const MojoDuplicateBufferHandleOptions* options,
    MojoHandle* new_buffer_handle) {
  RequestContext request_context;
  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(buffer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  // Don't verify |options| here; that's the dispatcher's job.
  scoped_refptr<Dispatcher> new_dispatcher;
  MojoResult result =
      dispatcher->DuplicateBufferHandle(options, &new_dispatcher);
  if (result != MOJO_RESULT_OK)
    return result;

  *new_buffer_handle = AddDispatcher(new_dispatcher);
  if (*new_buffer_handle == MOJO_HANDLE_INVALID) {
    LOG(ERROR) << "Handle table full";
    new_dispatcher->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::MapBuffer(MojoHandle buffer_handle,
                           uint64_t offset,
                           uint64_t num_bytes,
                           const MojoMapBufferOptions* options,
                           void** buffer) {
  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(buffer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (options->flags != MOJO_MAP_BUFFER_FLAG_NONE)
      return MOJO_RESULT_UNIMPLEMENTED;
  }

  std::unique_ptr<PlatformSharedMemoryMapping> mapping;
  MojoResult result = dispatcher->MapBuffer(offset, num_bytes, &mapping);
  if (result != MOJO_RESULT_OK)
    return result;

  DCHECK(mapping);
  void* address = mapping->GetBase();
  {
    base::AutoLock locker(mapping_table_lock_);
    if (mapping_table_.size() >= GetConfiguration().max_mapping_table_size)
      return MOJO_RESULT_RESOURCE_EXHAUSTED;
    auto emplace_result = mapping_table_.emplace(address, std::move(mapping));
    DCHECK(emplace_result.second);
  }

  *buffer = address;
  return MOJO_RESULT_OK;
}

MojoResult Core::UnmapBuffer(void* buffer) {
  std::unique_ptr<PlatformSharedMemoryMapping> mapping;
  // Destroy |mapping| while not holding the lock.
  {
    base::AutoLock lock(mapping_table_lock_);
    auto iter = mapping_table_.find(buffer);
    if (iter == mapping_table_.end())
      return MOJO_RESULT_INVALID_ARGUMENT;

    // Grab a reference so that it gets unmapped outside of this lock.
    mapping = std::move(iter->second);
    mapping_table_.erase(iter);
  }
  return MOJO_RESULT_OK;
}

MojoResult Core::GetBufferInfo(MojoHandle buffer_handle,
                               const MojoGetBufferInfoOptions* options,
                               MojoSharedBufferInfo* info) {
  if (options) {
    if (options->struct_size < sizeof(*options))
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (options->flags != MOJO_GET_BUFFER_INFO_FLAG_NONE)
      return MOJO_RESULT_UNIMPLEMENTED;
  }
  if (!info || info->struct_size < sizeof(MojoSharedBufferInfo))
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> dispatcher(GetDispatcher(buffer_handle));
  if (!dispatcher)
    return MOJO_RESULT_INVALID_ARGUMENT;

  return dispatcher->GetBufferInfo(info);
}

MojoResult Core::WrapPlatformHandle(
    const MojoPlatformHandle* platform_handle,
    const MojoWrapPlatformHandleOptions* options,
    MojoHandle* mojo_handle) {
  ScopedPlatformHandle handle;
  MojoResult result =
      MojoPlatformHandleToScopedPlatformHandle(platform_handle, &handle);
  if (result != MOJO_RESULT_OK)
    return result;

  return CreatePlatformHandleWrapper(std::move(handle), mojo_handle);
}

MojoResult Core::UnwrapPlatformHandle(
    MojoHandle mojo_handle,
    const MojoUnwrapPlatformHandleOptions* options,
    MojoPlatformHandle* platform_handle) {
  ScopedPlatformHandle handle;
  MojoResult result = PassWrappedPlatformHandle(mojo_handle, &handle);
  if (result != MOJO_RESULT_OK)
    return result;

  return ScopedPlatformHandleToMojoPlatformHandle(std::move(handle),
                                                  platform_handle);
}

MojoResult Core::WrapPlatformSharedMemoryRegion(
    const MojoPlatformHandle* platform_handles,
    uint32_t num_platform_handles,
    uint64_t size,
    const MojoSharedBufferGuid* guid,
    MojoPlatformSharedMemoryRegionAccessMode access_mode,
    const MojoWrapPlatformSharedMemoryRegionOptions* options,
    MojoHandle* mojo_handle) {
  DCHECK(size);

#if defined(OS_POSIX) && !defined(OS_ANDROID) && !defined(OS_FUCHSIA) && \
    (!defined(OS_MACOSX) || defined(OS_IOS))
  if (access_mode == MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_WRITABLE) {
    if (num_platform_handles != 2)
      return MOJO_RESULT_INVALID_ARGUMENT;
  }
#else
  if (num_platform_handles != 1)
    return MOJO_RESULT_INVALID_ARGUMENT;
#endif

  ScopedPlatformHandle handles[2];
  bool handles_ok = true;
  for (size_t i = 0; i < num_platform_handles; ++i) {
    MojoResult result = MojoPlatformHandleToScopedPlatformHandle(
        &platform_handles[i], &handles[i]);
    if (result != MOJO_RESULT_OK)
      handles_ok = false;
  }
  if (!handles_ok)
    return MOJO_RESULT_INVALID_ARGUMENT;

  base::UnguessableToken token =
      base::UnguessableToken::Deserialize(guid->high, guid->low);

  base::subtle::PlatformSharedMemoryRegion::Mode mode;
  switch (access_mode) {
    case MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_READ_ONLY:
      mode = base::subtle::PlatformSharedMemoryRegion::Mode::kReadOnly;
      break;
    case MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_WRITABLE:
      mode = base::subtle::PlatformSharedMemoryRegion::Mode::kWritable;
      break;
    case MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_UNSAFE:
      mode = base::subtle::PlatformSharedMemoryRegion::Mode::kUnsafe;
      break;
    default:
      return MOJO_RESULT_INVALID_ARGUMENT;
  }

  base::subtle::PlatformSharedMemoryRegion region =
      base::subtle::PlatformSharedMemoryRegion::Take(
          CreateSharedMemoryRegionHandleFromPlatformHandles(
              std::move(handles[0]), std::move(handles[1])),
          mode, size, token);
  if (!region.IsValid())
    return MOJO_RESULT_UNKNOWN;

  scoped_refptr<SharedBufferDispatcher> dispatcher;
  MojoResult result =
      SharedBufferDispatcher::CreateFromPlatformSharedMemoryRegion(
          std::move(region), &dispatcher);
  if (result != MOJO_RESULT_OK)
    return result;

  MojoHandle h = AddDispatcher(dispatcher);
  if (h == MOJO_HANDLE_INVALID) {
    dispatcher->Close();
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *mojo_handle = h;
  return MOJO_RESULT_OK;
}

MojoResult Core::UnwrapPlatformSharedMemoryRegion(
    MojoHandle mojo_handle,
    const MojoUnwrapPlatformSharedMemoryRegionOptions* options,
    MojoPlatformHandle* platform_handles,
    uint32_t* num_platform_handles,
    uint64_t* size,
    MojoSharedBufferGuid* guid,
    MojoPlatformSharedMemoryRegionAccessMode* access_mode) {
  scoped_refptr<Dispatcher> dispatcher;
  MojoResult result = MOJO_RESULT_OK;
  {
    base::AutoLock lock(handles_->GetLock());
    result = handles_->GetAndRemoveDispatcher(mojo_handle, &dispatcher);
    if (result != MOJO_RESULT_OK)
      return result;
  }

  if (dispatcher->GetType() != Dispatcher::Type::SHARED_BUFFER) {
    dispatcher->Close();
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  SharedBufferDispatcher* shm_dispatcher =
      static_cast<SharedBufferDispatcher*>(dispatcher.get());
  base::subtle::PlatformSharedMemoryRegion region =
      shm_dispatcher->PassPlatformSharedMemoryRegion();
  DCHECK(region.IsValid());
  DCHECK(size);
  *size = region.GetSize();

  base::UnguessableToken token = region.GetGUID();
  guid->high = token.GetHighForSerialization();
  guid->low = token.GetLowForSerialization();

  DCHECK(access_mode);
  switch (region.GetMode()) {
    case base::subtle::PlatformSharedMemoryRegion::Mode::kReadOnly:
      *access_mode = MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_READ_ONLY;
      break;
    case base::subtle::PlatformSharedMemoryRegion::Mode::kWritable:
      *access_mode = MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_WRITABLE;
      break;
    case base::subtle::PlatformSharedMemoryRegion::Mode::kUnsafe:
      *access_mode = MOJO_PLATFORM_SHARED_MEMORY_REGION_ACCESS_MODE_UNSAFE;
      break;
    default:
      return MOJO_RESULT_INVALID_ARGUMENT;
  }

  ScopedPlatformHandle handle;
  ScopedPlatformHandle read_only_handle;
  ExtractPlatformHandlesFromSharedMemoryRegionHandle(
      region.PassPlatformHandle(), &handle, &read_only_handle);

  const uint32_t available_handle_storage_slots = *num_platform_handles;
  if (available_handle_storage_slots < 1)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  *num_platform_handles = 1;
#if defined(OS_POSIX) && !defined(OS_ANDROID) && !defined(OS_FUCHSIA) && \
    (!defined(OS_MACOSX) || defined(OS_IOS))
  if (region.GetMode() ==
      base::subtle::PlatformSharedMemoryRegion::Mode::kWritable) {
    if (available_handle_storage_slots < 2)
      return MOJO_RESULT_INVALID_ARGUMENT;
    if (ScopedPlatformHandleToMojoPlatformHandle(std::move(read_only_handle),
                                                 &platform_handles[1]) !=
        MOJO_RESULT_OK) {
      return MOJO_RESULT_INVALID_ARGUMENT;
    }
    *num_platform_handles = 2;
  }
#endif

  if (ScopedPlatformHandleToMojoPlatformHandle(
          std::move(handle), &platform_handles[0]) != MOJO_RESULT_OK) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  return MOJO_RESULT_OK;
}

MojoResult Core::CreateInvitation(const MojoCreateInvitationOptions* options,
                                  MojoHandle* invitation_handle) {
  if (options && options->struct_size < sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (!invitation_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  *invitation_handle = AddDispatcher(new InvitationDispatcher);
  if (*invitation_handle == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  return MOJO_RESULT_OK;
}

MojoResult Core::AttachMessagePipeToInvitation(
    MojoHandle invitation_handle,
    uint64_t name,
    const MojoAttachMessagePipeToInvitationOptions* options,
    MojoHandle* message_pipe_handle) {
  if (options && options->struct_size < sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (!message_pipe_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> dispatcher = GetDispatcher(invitation_handle);
  if (!dispatcher || dispatcher->GetType() != Dispatcher::Type::INVITATION)
    return MOJO_RESULT_INVALID_ARGUMENT;
  auto* invitation_dispatcher =
      static_cast<InvitationDispatcher*>(dispatcher.get());

  RequestContext request_context;

  ports::PortRef remote_peer_port;
  MojoHandle local_handle = CreatePartialMessagePipe(&remote_peer_port);
  if (local_handle == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  MojoResult result = invitation_dispatcher->AttachMessagePipe(
      name, std::move(remote_peer_port));
  if (result != MOJO_RESULT_OK) {
    Close(local_handle);
    return result;
  }

  *message_pipe_handle = local_handle;
  return MOJO_RESULT_OK;
}

MojoResult Core::ExtractMessagePipeFromInvitation(
    MojoHandle invitation_handle,
    uint64_t name,
    const MojoExtractMessagePipeFromInvitationOptions* options,
    MojoHandle* message_pipe_handle) {
  if (options && options->struct_size < sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (!message_pipe_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;

  RequestContext request_context;

  scoped_refptr<Dispatcher> dispatcher = GetDispatcher(invitation_handle);
  if (!dispatcher || dispatcher->GetType() != Dispatcher::Type::INVITATION)
    return MOJO_RESULT_INVALID_ARGUMENT;
  auto* invitation_dispatcher =
      static_cast<InvitationDispatcher*>(dispatcher.get());
  // First attempt to extract from the invitation object itself. This is for
  // cases where this creation was created in-process.
  MojoResult extract_result =
      invitation_dispatcher->ExtractMessagePipe(name, message_pipe_handle);
  if (extract_result == MOJO_RESULT_OK ||
      extract_result == MOJO_RESULT_RESOURCE_EXHAUSTED) {
    return extract_result;
  }

  // Otherwise the invitation was created by a remote process, and we have to
  // asynchronously pair a new pipe handle with its attachment. See the other
  // |ExtractMessagePipeFromInvitation()| method.
  //
  // TODO(https://crbug.com/844231): Stop stringifying attachment names. This
  // can be done once the old EDK invitation API is removed.
  const std::string stringified_name = base::NumberToString(name);
  *message_pipe_handle = ExtractMessagePipeFromInvitation(stringified_name);
  if (*message_pipe_handle == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  return MOJO_RESULT_OK;
}

MojoResult Core::SendInvitation(
    MojoHandle invitation_handle,
    const MojoPlatformProcessHandle* process_handle,
    const MojoInvitationTransportEndpoint* transport_endpoint,
    MojoProcessErrorHandler error_handler,
    uintptr_t error_handler_context,
    const MojoSendInvitationOptions* options) {
  if (options && options->struct_size < sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  base::ProcessHandle target_process = base::kNullProcessHandle;
  if (process_handle) {
    if (process_handle->struct_size < sizeof(*process_handle))
      return MOJO_RESULT_INVALID_ARGUMENT;
#if defined(OS_WIN)
    target_process = reinterpret_cast<base::ProcessHandle>(
        static_cast<uintptr_t>(process_handle->value));
#else
    target_process = static_cast<base::ProcessHandle>(process_handle->value);
#endif
  }

  ProcessErrorCallback process_error_callback;
  if (error_handler) {
    process_error_callback = base::BindRepeating(
        &RunMojoProcessErrorHandler, GetNodeController()->io_task_runner(),
        error_handler, error_handler_context);
  } else if (default_process_error_callback_) {
    process_error_callback = default_process_error_callback_;
  }

  if (!transport_endpoint)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (transport_endpoint->struct_size < sizeof(*transport_endpoint))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (transport_endpoint->num_platform_handles == 0)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (!transport_endpoint->platform_handles)
    return MOJO_RESULT_INVALID_ARGUMENT;

  scoped_refptr<Dispatcher> dispatcher = GetDispatcher(invitation_handle);
  if (!dispatcher || dispatcher->GetType() != Dispatcher::Type::INVITATION)
    return MOJO_RESULT_INVALID_ARGUMENT;
  auto* invitation_dispatcher =
      static_cast<InvitationDispatcher*>(dispatcher.get());

  ScopedPlatformHandle endpoint_handle;
  MojoResult result = MojoPlatformHandleToScopedPlatformHandle(
      &transport_endpoint->platform_handles[0], &endpoint_handle);
  if (result != MOJO_RESULT_OK || !endpoint_handle.is_valid())
    return MOJO_RESULT_INVALID_ARGUMENT;

  // At this point everything else has been validated, so we can take ownership
  // of the dispatcher.
  {
    base::AutoLock lock(handles_->GetLock());
    scoped_refptr<Dispatcher> removed_dispatcher;
    MojoResult result = handles_->GetAndRemoveDispatcher(invitation_handle,
                                                         &removed_dispatcher);
    if (result != MOJO_RESULT_OK) {
      // Release ownership of the endpoint platform handle, per the API
      // contract. The caller retains ownership on failure.
      ignore_result(endpoint_handle.release());
      return result;
    }
    DCHECK_EQ(removed_dispatcher.get(), invitation_dispatcher);
  }

  ConnectionParams connection_params(TransportProtocol::kLegacy,
                                     std::move(endpoint_handle));

  // TODO(https://crbug.com/844231): Stop stringifying attachment names. This
  // can be done once the old EDK invitation API is removed.
  std::vector<std::pair<std::string, ports::PortRef>> attached_ports;
  InvitationDispatcher::PortMapping attached_port_map =
      invitation_dispatcher->TakeAttachedPorts();
  invitation_dispatcher->Close();
  for (auto& entry : attached_port_map) {
    attached_ports.emplace_back(base::NumberToString(entry.first),
                                std::move(entry.second));
  }

  RequestContext request_context;
  GetNodeController()->SendBrokerClientInvitation(
      target_process, std::move(connection_params), attached_ports,
      process_error_callback);

  return MOJO_RESULT_OK;
}

MojoResult Core::AcceptInvitation(
    const MojoInvitationTransportEndpoint* transport_endpoint,
    const MojoAcceptInvitationOptions* options,
    MojoHandle* invitation_handle) {
  if (options && options->struct_size < sizeof(*options))
    return MOJO_RESULT_INVALID_ARGUMENT;

  if (!transport_endpoint)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (transport_endpoint->struct_size < sizeof(*transport_endpoint))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (transport_endpoint->num_platform_handles == 0)
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (!transport_endpoint->platform_handles)
    return MOJO_RESULT_INVALID_ARGUMENT;

  if (!invitation_handle)
    return MOJO_RESULT_INVALID_ARGUMENT;
  *invitation_handle = AddDispatcher(new InvitationDispatcher);
  if (*invitation_handle == MOJO_HANDLE_INVALID)
    return MOJO_RESULT_RESOURCE_EXHAUSTED;

  ScopedPlatformHandle endpoint_handle;
  MojoResult result = MojoPlatformHandleToScopedPlatformHandle(
      &transport_endpoint->platform_handles[0], &endpoint_handle);
  if (result != MOJO_RESULT_OK) {
    Close(*invitation_handle);
    *invitation_handle = MOJO_HANDLE_INVALID;
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  RequestContext request_context;
  ConnectionParams connection_params(TransportProtocol::kLegacy,
                                     std::move(endpoint_handle));
  GetNodeController()->AcceptBrokerClientInvitation(
      std::move(connection_params));
  return MOJO_RESULT_OK;
}

void Core::GetActiveHandlesForTest(std::vector<MojoHandle>* handles) {
  base::AutoLock lock(handles_->GetLock());
  handles_->GetActiveHandlesForTest(handles);
}

// static
void Core::PassNodeControllerToIOThread(
    std::unique_ptr<NodeController> node_controller) {
  // It's OK to leak this reference. At this point we know the IO loop is still
  // running, and we know the NodeController will observe its eventual
  // destruction. This tells the NodeController to delete itself when that
  // happens.
  node_controller.release()->DestroyOnIOThreadShutdown();
}

}  // namespace edk
}  // namespace mojo
