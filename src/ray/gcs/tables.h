#ifndef RAY_GCS_TABLES_H
#define RAY_GCS_TABLES_H

#include <map>
#include <string>
#include <unordered_map>

#include "ray/constants.h"
#include "ray/id.h"
#include "ray/status.h"
#include "ray/util/logging.h"

#include "ray/gcs/format/gcs_generated.h"
#include "ray/gcs/redis_context.h"

// TODO(pcm): Remove this
#include "task.h"

struct redisAsyncContext;

namespace ray {

namespace gcs {

class RedisContext;

class AsyncGcsClient;

template <typename ID, typename Data>
class Table {
 public:
  using DataT = typename Data::NativeTableType;
  using Callback = std::function<
      void(AsyncGcsClient *client, const ID &id, std::shared_ptr<DataT> data)>;

  struct CallbackData {
    ID id;
    std::shared_ptr<DataT> data;
    Callback callback;
    Table<ID, Data> *table;
    AsyncGcsClient *client;
  };

  Table(const std::shared_ptr<RedisContext> &context, AsyncGcsClient *client)
      : context_(context), client_(client), pubsub_channel_(TablePubsub_NO_PUBLISH){};

  /// Add an entry to the table.
  ///
  /// \param job_id The ID of the job (= driver).
  /// \param id The ID of the data that is added to the GCS.
  /// \param data Data that is added to the GCS.
  /// \param done Callback that is called once the data has been written to the GCS.
  /// \return Status
  Status Add(const JobID &job_id,
             const ID &id,
             std::shared_ptr<DataT> data,
             const Callback &done) {
    auto d = std::shared_ptr<CallbackData>(
        new CallbackData({id, data, done, this, client_}));
    int64_t callback_index = RedisCallbackManager::instance().add([d](
        const std::string &data) { (d->callback)(d->client, d->id, d->data); });
    flatbuffers::FlatBufferBuilder fbb;
    fbb.ForceDefaults(true);
    fbb.Finish(Data::Pack(fbb, data.get()));
    RAY_RETURN_NOT_OK(context_->RunAsync("RAY.TABLE_ADD", id, fbb.GetBufferPointer(),
                                         fbb.GetSize(), pubsub_channel_, callback_index));
    return Status::OK();
  }

  /// Lookup an entry asynchronously.
  ///
  /// \param job_id The ID of the job (= driver).
  /// \param id The ID of the data that is looked up in the GCS.
  /// \param lookup Callback that is called after lookup.
  /// \return Status
  Status Lookup(const JobID &job_id, const ID &id, const Callback &lookup) {
    auto d = std::shared_ptr<CallbackData>(
        new CallbackData({id, nullptr, lookup, this}));
    int64_t callback_index =
        RedisCallbackManager::instance().add([d](const std::string &data) {
          auto result = std::make_shared<DataT>();
          auto root = flatbuffers::GetRoot<Data>(data.data());
          root->UnPackTo(result.get());
          (d->callback)(d->client, d->id, result);
        });
    std::vector<uint8_t> nil;
    RAY_RETURN_NOT_OK(context_->RunAsync("RAY.TABLE_LOOKUP", id, nil.data(), nil.size(),
                                         pubsub_channel_, callback_index));
    return Status::OK();
  }

  /// Subscribe to updates of this table
  ///
  /// \param job_id The ID of the job (= driver).
  /// \param client_id The type of update to listen to. If this is nil, then a
  ///        message for each Add to the table will be received. Else, only
  ///        messages for the given client will be received.
  /// \param subscribe Callback that is called on each received message.
  /// \param done Callback that is called when subscription is complete and we
  ///        are ready to receive messages..
  /// \return Status
  Status Subscribe(const JobID &job_id, const ClientID &client_id,
                   const Callback &subscribe, const Callback &done) {
    auto d = std::shared_ptr<CallbackData>(
        new CallbackData({client_id, nullptr, subscribe, this}));
    int64_t callback_index =
        RedisCallbackManager::instance().add([done, d](const std::string &data) {
          if (data.empty()) {
            // No data is provided. This is the callback for the initial
            // subscription request.
            done(d->client, d->id, nullptr);
          } else {
            auto result = std::make_shared<DataT>();
            auto root = flatbuffers::GetRoot<Data>(data.data());
            root->UnPackTo(result.get());
            (d->callback)(d->client, d->id, result);
          }
        });
    std::vector<uint8_t> nil;
    return context_->SubscribeAsync(client_id, pubsub_channel_, callback_index);
  }

  /// Remove and entry from the table
  Status Remove(const JobID &job_id, const ID &id, const Callback &done);

 protected:
  std::unordered_map<ID, std::unique_ptr<CallbackData>, UniqueIDHasher>
      callback_data_;
  std::shared_ptr<RedisContext> context_;
  AsyncGcsClient *client_;
  TablePubsub pubsub_channel_;
};

class ObjectTable : public Table<ObjectID, ObjectTableData> {
 public:
  ObjectTable(const std::shared_ptr<RedisContext> &context, AsyncGcsClient *client)
      : Table(context, client) {
    pubsub_channel_ = TablePubsub_OBJECT;
  };

  /// Set up a client-specific channel for receiving notifications about
  /// available
  /// objects from the object table. The callback will be called once per
  /// notification received on this channel.
  ///
  /// \param subscribe_all
  /// \param object_available_callback Callback to be called when new object
  ///        becomes available.
  /// \param done_callback Callback to be called when subscription is installed.
  ///        This is only used for the tests.
  /// \return Status
  Status SubscribeToNotifications(const JobID &job_id,
                                  bool subscribe_all,
                                  const Callback &object_available,
                                  const Callback &done);

  /// Request notifications about the availability of some objects from the
  /// object
  /// table. The notifications will be published to this client's object
  /// notification channel, which was set up by the method
  /// ObjectTableSubscribeToNotifications.
  ///
  /// \param object_ids The object IDs to receive notifications about.
  /// \return Status
  Status RequestNotifications(const JobID &job_id,
                              const std::vector<ObjectID> &object_ids);
};

using FunctionTable = Table<FunctionID, FunctionTableData>;

using ClassTable = Table<ClassID, ClassTableData>;

// TODO(swang): Set the pubsub channel for the actor table.
using ActorTable = Table<ActorID, ActorTableData>;

class TaskTable : public Table<TaskID, TaskTableData> {
 public:
  TaskTable(const std::shared_ptr<RedisContext> &context, AsyncGcsClient *client)
      : Table(context, client) {
    pubsub_channel_ = TablePubsub_TASK;
  };

  using TestAndUpdateCallback = std::function<void(AsyncGcsClient *client,
                                                   const TaskID &id,
                                                   const TaskTableDataT &task,
                                                   bool updated)>;
  using SubscribeToTaskCallback =
      std::function<void(std::shared_ptr<TaskTableDataT> task)>;
  /// Update a task's scheduling information in the task table, if the current
  /// value matches the given test value. If the update succeeds, it also
  /// updates
  /// the task entry's local scheduler ID with the ID of the client who called
  /// this function. This assumes that the task spec already exists in the task
  /// table entry.
  ///
  /// \param task_id The task ID of the task entry to update.
  /// \param test_state_bitmask The bitmask to apply to the task entry's current
  ///        scheduling state.  The update happens if and only if the current
  ///        scheduling state AND-ed with the bitmask is greater than 0.
  /// \param update_state The value to update the task entry's scheduling state
  ///        with, if the current state matches test_state_bitmask.
  /// \param callback Function to be called when database returns result.
  /// \return Status
  Status TestAndUpdate(const JobID &job_id,
                       const TaskID &id,
                       std::shared_ptr<TaskTableTestAndUpdateT> data,
                       const TestAndUpdateCallback &callback) {
    int64_t callback_index = RedisCallbackManager::instance().add(
        [this, callback, id](const std::string &data) {
          auto result = std::make_shared<TaskTableDataT>();
          auto root = flatbuffers::GetRoot<TaskTableData>(data.data());
          root->UnPackTo(result.get());
          callback(client_, id, *result, root->updated());
        });
    flatbuffers::FlatBufferBuilder fbb;
    TaskTableTestAndUpdateBuilder builder(fbb);
    fbb.Finish(TaskTableTestAndUpdate::Pack(fbb, data.get()));
    RAY_RETURN_NOT_OK(context_->RunAsync("RAY.TABLE_TEST_AND_UPDATE", id,
                                         fbb.GetBufferPointer(), fbb.GetSize(),
                                         pubsub_channel_, callback_index));
    return Status::OK();
  }

  /// This has a separate signature from Subscribe in Table
  /// Register a callback for a task event. An event is any update of a task in
  /// the task table.
  /// Events include changes to the task's scheduling state or changes to the
  /// task's local scheduler ID.
  ///
  /// \param local_scheduler_id The db_client_id of the local scheduler whose
  ///        events we want to listen to. If you want to subscribe to updates
  ///        from
  ///        all local schedulers, pass in NIL_ID.
  /// \param subscribe_callback Callback that will be called when the task table is
  ///        updated.
  /// \param state_filter Events we want to listen to. Can have values from the
  ///        enum "scheduling_state" in task.h.
  ///        TODO(pcm): Make it possible to combine these using flags like
  ///        TASK_STATUS_WAITING | TASK_STATUS_SCHEDULED.
  /// \param callback Function to be called when database returns result.
  /// \return Status
  Status SubscribeToTask(const JobID &job_id, const ClientID &local_scheduler_id,
                         int state_filter, const SubscribeToTaskCallback &callback,
                         const Callback &done);
};

using ErrorTable = Table<TaskID, ErrorTableData>;

using CustomSerializerTable = Table<ClassID, CustomSerializerData>;

using ConfigTable = Table<ConfigID, ConfigTableData>;

Status TaskTableAdd(AsyncGcsClient *gcs_client, Task *task);

Status TaskTableTestAndUpdate(AsyncGcsClient *gcs_client, const TaskID &task_id,
                              const ClientID &local_scheduler_id, int test_state_bitmask,
                              SchedulingState update_state,
                              const TaskTable::TestAndUpdateCallback &callback);

/// \class ClientInformation
///
/// Represents information in the client table about a particular client. Each
/// client has an associated node manager.
class ClientInformation {
 public:
  /// Create a client information object.
  ///
  /// \param client_table_entry A serialized client table entry flatbuffer.
  ClientInformation(const ClientTableData &client_table_entry);

  /// Get the client ID.
  ///
  /// \return The ID of this client.
  const ClientID &GetClientId() const;

  /// Get the IP address of the client's node manager.
  ///
  /// \return The IP address of the client's node manager.
  const std::string GetIpAddress() const;

  /// Get the port at which the client's node manager is listening for
  /// TCP connections.
  ///
  /// \return The client's TCP port.
  int GetPort() const;

  /// Get whether the client is alive.
  ///
  /// \return Whether the client is alive.
  bool IsAlive() const;
};

class ClientTable : private Table<ClientID, ClientTableData> {
 public:
  ClientTable(const std::shared_ptr<RedisContext> &context, AsyncGcsClient *client);

  /// Connect as a client to the GCS. This registers us in the client table and
  /// begins subscription to client table notifications.
  ///
  /// \param[out] client_id The assigned client ID will be written to this pointer.
  /// \return Status
  // TODO(swang): Call this from AsyncGcsClient::Connect?
  ray::Status Connect(ClientID *client_id);

  /// Disconnect the client from the GCS. The client ID assigned during
  /// registration should never be reused after disconnecting.
  ///
  /// \return Status
  ray::Status Disconnect();

  /// Get a client's information from the cache.
  ///
  /// \param client The client to get information about.
  const ClientInformation &GetClientInformation(const ClientID &client);

 private:
  /// This client's ID.
  ClientID client_id_;
  /// A cache for information about all clients.
  std::unordered_map<ClientID, ClientInformation, UniqueIDHasher> client_cache_;
};

}  // namespace gcs

}  // namespace ray

#endif  // RAY_GCS_TABLES_H
