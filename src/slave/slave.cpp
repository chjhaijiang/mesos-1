#include <errno.h>

#include <algorithm>

#include <process/timer.hpp>

#include "slave.hpp"

#include "common/build.hpp"
#include "common/type_utils.hpp"
#include "common/utils.hpp"

using namespace process;

using std::string;


namespace mesos { namespace internal { namespace slave {

// Information describing an executor (goes away if executor crashes).
struct Executor
{
  Executor(const FrameworkID& _frameworkId,
           const ExecutorInfo& _info,
           const string& _directory)
    : frameworkId(_frameworkId),
      info(_info),
      directory(_directory),
      id(_info.executor_id()),
      pid(UPID()) {}

  virtual ~Executor()
  {
    // Delete the tasks.
    foreachvalue (Task* task, launchedTasks) {
      delete task;
    }
  }

  Task* addTask(const TaskDescription& task)
  {
    // The master should enforce unique task IDs, but just in case
    // maybe we shouldn't make this a fatal error.
    CHECK(!launchedTasks.contains(task.task_id()));

    Task *t = new Task();
    t->mutable_framework_id()->MergeFrom(frameworkId);
    t->mutable_executor_id()->MergeFrom(id);
    t->set_state(TASK_STARTING);
    t->set_name(task.name());
    t->mutable_task_id()->MergeFrom(task.task_id());
    t->mutable_slave_id()->MergeFrom(task.slave_id());
    t->mutable_resources()->MergeFrom(task.resources());

    launchedTasks[task.task_id()] = t;
    resources += task.resources();
  }

  void removeTask(const TaskID& taskId)
  {
    // Remove the task if it's queued.
    queuedTasks.erase(taskId);

    // Update the resources if it's been launched.
    if (launchedTasks.contains(taskId)) {
      Task* task = launchedTasks[taskId];
      foreach (const Resource& resource, task->resources()) {
        resources -= resource;
      }
      launchedTasks.erase(taskId);
      delete task;
    }
  }

  void updateTaskState(const TaskID& taskId, TaskState state)
  {
    if (launchedTasks.contains(taskId)) {
      launchedTasks[taskId]->set_state(state);
    }
  }

  const ExecutorID id;
  const ExecutorInfo info;

  const FrameworkID frameworkId;

  const string directory;

  UPID pid;

  Resources resources;

  hashmap<TaskID, TaskDescription> queuedTasks;
  hashmap<TaskID, Task*> launchedTasks;
};


// Information about a framework.
struct Framework
{
  Framework(const FrameworkID& _id,
            const FrameworkInfo& _info,
            const UPID& _pid)
    : id(_id), info(_info), pid(_pid) {}

  virtual ~Framework() {}

  Executor* createExecutor(const ExecutorInfo& executorInfo,
                           const string& directory)
  {
    Executor* executor = new Executor(id, executorInfo, directory);
    CHECK(!executors.contains(executorInfo.executor_id()));
    executors[executorInfo.executor_id()] = executor;
    return executor;
  }

  void destroyExecutor(const ExecutorID& executorId)
  {
    if (executors.contains(executorId)) {
      Executor* executor = executors[executorId];
      executors.erase(executorId);
      delete executor;
    }
  }

  Executor* getExecutor(const ExecutorID& executorId)
  {
    if (executors.contains(executorId)) {
      return executors[executorId];
    }

    return NULL;
  }

  Executor* getExecutor(const TaskID& taskId)
  {
    foreachvalue (Executor* executor, executors) {
      if (executor->queuedTasks.contains(taskId) ||
          executor->launchedTasks.contains(taskId)) {
        return executor;
      }
    }

    return NULL;
  }

  const FrameworkID id;
  const FrameworkInfo info;

  UPID pid;

  hashmap<ExecutorID, Executor*> executors;
  hashmap<TaskID, StatusUpdate> updates;
};


// // Represents a pending status update that has been sent and we are
// // waiting for an acknowledgement. In pa

// // stream of status updates for a framework/task. Note
// // that these are stored in the slave rather than per Framework
// // because a framework might go away before all of the status
// // updates have been sent and acknowledged.
// struct Slave::StatusUpdateStream
// {
//   StatusUpdateStreamID streamId;
//   string directory;
//   FILE* updates;
//   FILE* acknowledged;
//   queue<StatusUpdate> pending;
//   double timeout;
// };


//   StatusUpdateStreamID id;



//   queue<StatusUpdate> pending;
//   double timeout;
// };


Slave::Slave(const Configuration& _conf,
             bool _local,
             IsolationModule* _isolationModule)
  : ProtobufProcess<Slave>("slave"),
    conf(_conf),
    local(_local),
    isolationModule(_isolationModule)
{
  resources =
    Resources::parse(conf.get<string>("resources", "cpus:1;mem:1024"));

  initialize();
}


Slave::Slave(const Resources& _resources,
             bool _local,
             IsolationModule *_isolationModule)
  : ProtobufProcess<Slave>("slave"),
    resources(_resources),
    local(_local),
    isolationModule(_isolationModule)
{
  initialize();
}


Slave::~Slave()
{
  // TODO(benh): Shut down and free frameworks?

  // TODO(benh): Shut down and free executors? The executor should get
  // an "exited" event and initiate shutdown itself.
}


void Slave::registerOptions(Configurator* configurator)
{
  // TODO(benh): Is there a way to specify units for the resources?
  configurator->addOption<string>(
      "resources",
      "Total consumable resources per slave\n");

  configurator->addOption<string>(
      "attributes",
      "Attributes of machine\n");

  configurator->addOption<string>(
      "work_dir",
      "Where to place framework work directories\n"
      "(default: MESOS_HOME/work)");

  configurator->addOption<string>(
      "hadoop_home",
      "Where to find Hadoop installed (for\n"
      "fetching framework executors from HDFS)\n"
      "(default: look for HADOOP_HOME in\n"
      "environment or find hadoop on PATH)");

  configurator->addOption<bool>(
      "switch_user", 
      "Whether to run tasks as the user who\n"
      "submitted them rather than the user running\n"
      "the slave (requires setuid permission)",
      true);

  configurator->addOption<string>(
      "frameworks_home",
      "Directory prepended to relative executor\n"
      "paths (default: MESOS_HOME/frameworks)");
}


Promise<state::SlaveState*> Slave::getState()
{
  Resources resources(this->resources);
  Resource::Scalar cpus;
  Resource::Scalar mem;
  cpus.set_value(0);
  mem.set_value(0);
  cpus = resources.getScalar("cpus", cpus);
  mem = resources.getScalar("mem", mem);

  state::SlaveState* state = new state::SlaveState(
      build::DATE, build::USER, id.value(),
      cpus.value(), mem.value(), self(), master);

  foreachvalue (Framework* f, frameworks) {
    foreachvalue (Executor* e, f->executors) {
      Resources resources(e->resources);
      Resource::Scalar cpus;
      Resource::Scalar mem;
      cpus.set_value(0);
      mem.set_value(0);
      cpus = resources.getScalar("cpus", cpus);
      mem = resources.getScalar("mem", mem);

      // TOOD(benh): For now, we will add a state::Framework object
      // for each executor that the framework has. Therefore, we tweak
      // the framework ID to also include the associated executor ID
      // to differentiate them. This is so we don't have to make very
      // many changes to the webui right now. Note that this ID
      // construction must be identical to what we do for directory
      // suffix returned from Slave::getUniqueWorkDirectory.

      string id = f->id.value() + "-" + e->id.value();

      state::Framework* framework = new state::Framework(
          id, f->info.name(),
          e->info.uri(), "",
          cpus.value(), mem.value());

      state->frameworks.push_back(framework);

      foreachvalue (Task* t, e->launchedTasks) {
        Resources resources(t->resources());
        Resource::Scalar cpus;
        Resource::Scalar mem;
        cpus.set_value(0);
        mem.set_value(0);
        cpus = resources.getScalar("cpus", cpus);
        mem = resources.getScalar("mem", mem);

        state::Task* task = new state::Task(
            t->task_id().value(), t->name(),
            TaskState_Name(t->state()),
            cpus.value(), mem.value());

        framework->tasks.push_back(task);
      }
    }
  }

  return state;
}


void Slave::initialize()
{
  // Start all the statistics at 0.
  CHECK(TASK_STARTING == TaskState_MIN);
  CHECK(TASK_LOST == TaskState_MAX);
  stats.tasks[TASK_STARTING] = 0;
  stats.tasks[TASK_RUNNING] = 0;
  stats.tasks[TASK_FINISHED] = 0;
  stats.tasks[TASK_FAILED] = 0;
  stats.tasks[TASK_KILLED] = 0;
  stats.tasks[TASK_LOST] = 0;
  stats.validStatusUpdates = 0;
  stats.invalidStatusUpdates = 0;
  stats.validFrameworkMessages = 0;
  stats.invalidFrameworkMessages = 0;

  startTime = elapsedTime();

  // Install protobuf handlers.
  installProtobufHandler<NewMasterDetectedMessage>(
      &Slave::newMasterDetected,
      &NewMasterDetectedMessage::pid);

  installProtobufHandler<NoMasterDetectedMessage>(
      &Slave::noMasterDetected);

  installProtobufHandler<SlaveRegisteredMessage>(
      &Slave::registered,
      &SlaveRegisteredMessage::slave_id);

  installProtobufHandler<SlaveReregisteredMessage>(
      &Slave::reregistered,
      &SlaveReregisteredMessage::slave_id);

  installProtobufHandler<RunTaskMessage>(
      &Slave::runTask,
      &RunTaskMessage::framework,
      &RunTaskMessage::framework_id,
      &RunTaskMessage::pid,
      &RunTaskMessage::task);

  installProtobufHandler<KillTaskMessage>(
      &Slave::killTask,
      &KillTaskMessage::framework_id,
      &KillTaskMessage::task_id);

  installProtobufHandler<KillFrameworkMessage>(
      &Slave::killFramework,
      &KillFrameworkMessage::framework_id);

  installProtobufHandler<FrameworkToExecutorMessage>(
      &Slave::schedulerMessage,
      &FrameworkToExecutorMessage::slave_id,
      &FrameworkToExecutorMessage::framework_id,
      &FrameworkToExecutorMessage::executor_id,
      &FrameworkToExecutorMessage::data);

  installProtobufHandler<UpdateFrameworkMessage>(
      &Slave::updateFramework,
      &UpdateFrameworkMessage::framework_id,
      &UpdateFrameworkMessage::pid);

  installProtobufHandler<StatusUpdateAcknowledgementMessage>(
      &Slave::statusUpdateAcknowledgement,
      &StatusUpdateAcknowledgementMessage::slave_id,
      &StatusUpdateAcknowledgementMessage::framework_id,
      &StatusUpdateAcknowledgementMessage::task_id);

  installProtobufHandler<RegisterExecutorMessage>(
      &Slave::registerExecutor,
      &RegisterExecutorMessage::framework_id,
      &RegisterExecutorMessage::executor_id);

  installProtobufHandler<StatusUpdateMessage>(
      &Slave::statusUpdate,
      &StatusUpdateMessage::update);

  installProtobufHandler<ExecutorToFrameworkMessage>(
      &Slave::executorMessage,
      &ExecutorToFrameworkMessage::slave_id,
      &ExecutorToFrameworkMessage::framework_id,
      &ExecutorToFrameworkMessage::executor_id,
      &ExecutorToFrameworkMessage::data);

  // Install some message handlers.
  installMessageHandler(process::EXITED, &Slave::exited);
  installMessageHandler("PING", &Slave::ping);

  // Install some HTTP handlers.
  installHttpHandler("info.json", &Slave::http_info_json);
  installHttpHandler("frameworks.json", &Slave::http_frameworks_json);
  installHttpHandler("tasks.json", &Slave::http_tasks_json);
  installHttpHandler("stats.json", &Slave::http_stats_json);
  installHttpHandler("vars", &Slave::http_vars);
}


void Slave::operator () ()
{
  LOG(INFO) << "Slave started at " << self();
  LOG(INFO) << "Slave resources: " << resources;

  Result<string> result = utils::os::hostname();

  if (result.isError()) {
    LOG(FATAL) << "Failed to get hostname: " << result.error();
  }

  CHECK(result.isSome());

  string hostname = result.get();

  // Check and see if we have a different public DNS name. Normally
  // this is our hostname, but on EC2 we look for the MESOS_PUBLIC_DNS
  // environment variable. This allows the master to display our
  // public name in its web UI.
  string public_hostname = hostname;
  if (getenv("MESOS_PUBLIC_DNS") != NULL) {
    public_hostname = getenv("MESOS_PUBLIC_DNS");
  }

  // Initialize slave info.
  info.set_hostname(hostname);
  info.set_public_hostname(public_hostname);
  info.mutable_resources()->MergeFrom(resources);

  // Spawn and initialize the isolation module.
  spawn(isolationModule);
  dispatch(isolationModule,
           &IsolationModule::initialize,
           conf, local, self());

  while (true) {
    serve(1);
    if (name() == process::TERMINATE) {
      LOG(INFO) << "Asked to terminate by " << from();
      foreachvalue (Framework* framework, utils::copy(frameworks)) {
        removeFramework(framework);
      }
      break;
    }
  }

  // Stop the isolation module.
  terminate(isolationModule->self());
  wait(isolationModule->self());
}


void Slave::newMasterDetected(const string& pid)
{
  LOG(INFO) << "New master detected at " << pid;

  master = pid;
  link(master);

  if (id == "") {
    // Slave started before master.
    RegisterSlaveMessage message;
    message.mutable_slave()->MergeFrom(info);
    send(master, message);
  } else {
    // Re-registering, so send tasks running.
    ReregisterSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(id);
    message.mutable_slave()->MergeFrom(info);

    foreachvalue (Framework* framework, frameworks) {
      foreachvalue (Executor* executor, framework->executors) {
	foreachvalue (Task* task, executor->launchedTasks) {
          // TODO(benh): Also need to send queued tasks here ...
	  message.add_tasks()->MergeFrom(*task);
	}
      }
    }

    send(master, message);
  }
}


void Slave::noMasterDetected()
{
  LOG(INFO) << "Lost master(s) ... waiting";
}


void Slave::registered(const SlaveID& slaveId)
{
  LOG(INFO) << "Registered with master; given slave ID " << slaveId;
  id = slaveId;
}


void Slave::reregistered(const SlaveID& slaveId)
{
  LOG(INFO) << "Re-registered with master";

  if (!(id == slaveId)) {
    LOG(FATAL) << "Slave re-registered but got wrong ID";
  }
}


void Slave::runTask(const FrameworkInfo& frameworkInfo,
                    const FrameworkID& frameworkId,
                    const string& pid,
                    const TaskDescription& task)
{
  LOG(INFO) << "Got assigned task " << task.task_id()
            << " for framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    framework = new Framework(frameworkId, frameworkInfo, pid);
    frameworks[frameworkId] = framework;
  }

  // Either send the task to an executor or start a new executor
  // and queue the task until the executor has started.
  Executor* executor = task.has_executor()
    ? framework->getExecutor(task.executor().executor_id())
    : framework->getExecutor(framework->info.executor().executor_id());
        
  if (executor != NULL) {
    if (!executor->pid) {
      // Queue task until the executor starts up.
      executor->queuedTasks[task.task_id()] = task;
    } else {
      // Add the task and send it to the executor.
      executor->addTask(task);

      stats.tasks[TASK_STARTING]++;

      RunTaskMessage message;
      message.mutable_framework()->MergeFrom(framework->info);
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(task);
      send(executor->pid, message);

      // Now update the resources.
      dispatch(isolationModule,
               &IsolationModule::resourcesChanged,
               framework->id, executor->id, executor->resources);
    }
  } else {
    // Launch an executor for this task.
    ExecutorInfo executorInfo = task.has_executor()
      ? task.executor()
      : framework->info.executor();

    ExecutorID executorId = executorInfo.executor_id();

    string directory = getUniqueWorkDirectory(framework->id, executorId);

    LOG(INFO) << "Using '" << directory
              << "' as work directory for executor '" << executorId
              << "' of framework " << framework->id;

    executor = framework->createExecutor(executorInfo, directory);

    // Queue task until the executor starts up.
    executor->queuedTasks[task.task_id()] = task;

    // Tell the isolation module to launch the executor. (TODO(benh):
    // Make the isolation module a process so that it can block while
    // trying to launch the executor.)
    dispatch(isolationModule,
             &IsolationModule::launchExecutor,
             framework->id, framework->info, executor->info, directory);

  }
}


void Slave::killTask(const FrameworkID& frameworkId,
                     const TaskID& taskId)
{
  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such framework is running";

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus *status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_LOST);
    update->set_timestamp(elapsedTime());
    update->set_sequence(-1);
    message.set_reliable(false);
    send(master, message);

    return;
  }


  // Tell the executor to kill the task if it is up and
  // running, otherwise, consider the task lost.
  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such task is running";

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(framework->id);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus *status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_LOST);
    update->set_timestamp(elapsedTime());
    update->set_sequence(-1);
    message.set_reliable(false);
    send(master, message);
  } else if (!executor->pid) {
    // Remove the task.
    executor->removeTask(taskId);

    // Tell the isolation module to update the resources.
    dispatch(isolationModule,
             &IsolationModule::resourcesChanged,
             framework->id, executor->id, executor->resources);

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(framework->id);
    update->mutable_executor_id()->MergeFrom(executor->id);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus *status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_KILLED);
    update->set_timestamp(elapsedTime());
    update->set_sequence(0);
    message.set_reliable(false);
    send(master, message);
  } else {
    // Otherwise, send a message to the executor and wait for
    // it to send us a status update.
    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_task_id()->MergeFrom(taskId);
    send(executor->pid, message);
  }
}


void Slave::killFramework(const FrameworkID& frameworkId)
{
  LOG(INFO) << "Asked to kill framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    removeFramework(framework);
  }
}


void Slave::schedulerMessage(const SlaveID& slaveId,
			     const FrameworkID& frameworkId,
			     const ExecutorID& executorId,
                             const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Dropping message for framework "<< frameworkId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor does not exist";
    stats.invalidFrameworkMessages++;
  } else if (!executor->pid) {
    // TODO(*): If executor is not started, queue framework message?
    // (It's probably okay to just drop it since frameworks can have
    // the executor send a message to the master to say when it's ready.)
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor is not running";
    stats.invalidFrameworkMessages++;
  } else {
    FrameworkToExecutorMessage message;
    message.mutable_slave_id()->MergeFrom(slaveId);
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_data(data);
    send(executor->pid, message);

    stats.validFrameworkMessages++;
  }
}


void Slave::updateFramework(const FrameworkID& frameworkId,
                            const string& pid)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    LOG(INFO) << "Updating framework " << frameworkId
              << " pid to " <<pid;
    framework->pid = pid;
  }
}


void Slave::statusUpdateAcknowledgement(const SlaveID& slaveId,
                                        const FrameworkID& frameworkId,
                                        const TaskID& taskId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->updates.contains(taskId)) {
      // TODO(benh): Check sequence!
      LOG(INFO) << "Got acknowledgement of status update"
                << " for task " << taskId
                << " of framework " << framework->id;
      framework->updates.erase(taskId);
    }
  }
}


// void Slave::statusUpdateAcknowledged(const SlaveID& slaveId,
//                                      const FrameworkID& frameworkId,
//                                      const TaskID& taskId,
//                                      uint32_t sequence)
// {
//   StatusUpdateStreamID id(frameworkId, taskId);
//   StatusUpdateStream* stream = getStatusUpdateStream(id);

//   if (stream == NULL) {
//     LOG(WARNING) << "WARNING! Received unexpected status update"
//                  << " acknowledgement for task " << taskId
//                  << " of framework " << frameworkId;
//     return;
//   }

//   CHECK(!stream->pending.empty());

//   const StatusUpdate& update = stream->pending.front();

//   if (update->sequence() != sequence) {
//     LOG(WARNING) << "WARNING! Received status update acknowledgement"
//                  << " with bad sequence number (received " << sequence
//                  << ", expecting " << update->sequence()
//                  << ") for task " << taskId
//                  << " of framework " << frameworkId;
//   } else {
//     LOG(INFO) << "Received status update acknowledgement for task "
//               << taskId << " of framework " << frameworkId;

//     // Write the update out to disk.
//     CHECK(stream->acknowledged != NULL);

//     Result<bool> result =
//       utils::protobuf::write(stream->acknowledged, update);

//     if (result.isError()) {
//       // Failing here is rather dramatic, but so is not being able to
//       // write to disk ... seems like failing early and often might do
//       // more benefit than harm.
//       LOG(FATAL) << "Failed to write status update to "
//                  << stream->directory << "/acknowledged: "
//                  << result.message();
//     }

//     stream->pending.pop();

//     bool empty = stream->pending.empty();

//     bool terminal =
//       update.status().state() == TASK_FINISHED &&
//       update.status().state() == TASK_FAILED &&
//       update.status().state() == TASK_KILLED &&
//       update.status().state() == TASK_LOST;

//     if (empty && terminal) {
//       cleanupStatusUpdateStream(stream);
//     } else if (!empty && terminal) {
//       LOG(WARNING) << "WARNING! Acknowledged a \"terminal\""
//                    << " task status but updates are still pending";
//     } else if (!empty) {
//       StatusUpdateMessage message;
//       message.mutable_update()->MergeFrom(stream->pending.front());
//       message.set_reliable(true);
//       send(master, message);

//       stream->timeout = elapsedTime() + STATUS_UPDATE_RETRY_INTERVAL;
//     }
//   }
// }


void Slave::registerExecutor(const FrameworkID& frameworkId,
                             const ExecutorID& executorId)
{
  LOG(INFO) << "Got registration for executor '" << executorId
            << "' of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    // Framework is gone; tell the executor to exit.
    LOG(WARNING) << "Framework " << frameworkId
                 << " does not exist (it may have been killed),"
                 << " telling executor to exit";
    send(from(), ShutdownMessage());
    return;
  }

  Executor* executor = framework->getExecutor(executorId);

  // Check the status of the executor.
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Unexpected executor '" << executorId
                 << "' registering for framework " << frameworkId;
    send(from(), ShutdownMessage());
  } else if (executor->pid != UPID()) {
    LOG(WARNING) << "WARNING! executor '" << executorId
                 << "' of framework " << frameworkId
                 << " is already running";
    send(from(), ShutdownMessage());
  } else {
    // Save the pid for the executor.
    executor->pid = from();

    // Now that the executor is up, set its resource limits.
    dispatch(isolationModule,
             &IsolationModule::resourcesChanged,
             framework->id, executor->id, executor->resources);

    // Tell executor it's registered and give it any queued tasks.
    ExecutorRegisteredMessage message;
    ExecutorArgs* args = message.mutable_args();
    args->mutable_framework_id()->MergeFrom(framework->id);
    args->mutable_executor_id()->MergeFrom(executor->id);
    args->mutable_slave_id()->MergeFrom(id);
    args->set_hostname(info.hostname());
    args->set_data(executor->info.data());
    send(executor->pid, message);

    LOG(INFO) << "Flushing queued tasks for framework " << framework->id;

    foreachvalue (const TaskDescription& task, executor->queuedTasks) {
      // Add the task to the executor.
      executor->addTask(task);

      stats.tasks[TASK_STARTING]++;

      RunTaskMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_framework()->MergeFrom(framework->info);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(task);
      send(executor->pid, message);
    }

    executor->queuedTasks.clear();
  }
}


// void Slave::statusUpdate(const StatusUpdate& update)
// {
//   LOG(INFO) << "Received update that task " << update.status().task_id()
//             << " of framework " << update.framework_id()
//             << " is now in state " << update.status().state();

//   Framework* framework = getFramework(update.framework_id());
//   if (framework == NULL) {
//     LOG(WARNING) << "WARNING! Failed to lookup"
//                  << " framework " << update.framework_id()
//                  << " of received status update";
//     stats.invalidStatusUpdates++;
//     return;
//   }

//   Executor* executor = framework->getExecutor(update.status().task_id());
//   if (executor == NULL) {
//     LOG(WARNING) << "WARNING! Failed to lookup executor"
//                  << " for framework " << update.framework_id()
//                  << " of received status update";
//     stats.invalidStatusUpdates++;
//     return;
//   }

//   // Create/Get the status update stream for this framework/task.
//   StatusUpdateStreamID id(update.framework_id(), update.status().task_id());

//   if (!statusUpdateStreams.contains(id)) {
//     StatusUpdateStream* stream =
//       createStatusUpdateStream(id, executor->directory);

//     if (stream == NULL) {
//       LOG(WARNING) << "WARNING! Failed to create status update"
//                    << " stream for task " << update.status().task_id()
//                    << " of framework " << update.framework_id()
//                    << " ... removing executor!";
//       removeExecutor(framework, executor);
//       return;
//     }
//   }

//   StatusUpdateStream* stream = getStatusUpdateStream(id);

//   CHECK(stream != NULL);

//   // If we are already waiting on an acknowledgement, check that this
//   // update (coming from the executor), is the same one that we are
//   // waiting on being acknowledged.

//   // Check that this is status update has not already been
//   // acknowledged. this could happen because a slave writes the
//   // acknowledged message but then fails before it can pass the
//   // message on to the executor, so the executor tries again.

//   returnhere;

//   // TODO(benh): Check that this update hasn't already been received
//   // or acknowledged! This could happen if a slave receives a status
//   // update from an executor, then crashes after it writes it to disk
//   // but before it sends an ack back to 

//   // Okay, record this update as received.
//   CHECK(stream->received != NULL);

//   Result<bool> result =
//     utils::protobuf::write(stream->received, &update);

//   if (result.isError()) {
//     // Failing here is rather dramatic, but so is not being able to
//     // write to disk ... seems like failing early and often might do
//     // more benefit than harm.
//     LOG(FATAL) << "Failed to write status update to "
//                << stream->directory << "/received: "
//                << result.message();
//   }

//   // Now acknowledge the executor.
//   StatusUpdateAcknowledgementMessage message;
//   message.mutable_framework_id()->MergeFrom(update.framework_id());
//   message.mutable_slave_id()->MergeFrom(update.slave_id());
//   message.mutable_task_id()->MergeFrom(update.status().task_id());
//   send(executor->pid, message);

//   executor->updateTaskState(
//       update.status().task_id(),
//       update.status().state());

//   // Remove the task if it's reached a terminal state.
//   bool terminal =
//     update.status().state() == TASK_FINISHED &&
//     update.status().state() == TASK_FAILED &&
//     update.status().state() == TASK_KILLED &&
//     update.status().state() == TASK_LOST;

//   if (terminal) {
//     executor->removeTask(update.status().task_id());
//     isolationModule->resourcesChanged(
//         framework->id, framework->info,
//         executor->info, executor->resources);
//   }

//   stream->pending.push(update);

//   // Send the status update if this is the first in the
//   // stream. Subsequent status updates will get sent in
//   // Slave::statusUpdateAcknowledged.
//   if (stream->pending.size() == 1) {
//     CHECK(stream->timeout == -1);
//     StatusUpdateMessage message;
//     message.mutable_update()->MergeFrom(update);
//     message.set_reliable(true);
//     send(master, message);

//     stream->timeout = elapsedTime() + STATUS_UPDATE_RETRY_INTERVAL;
//   }

//   stats.tasks[status.state()]++;
//   stats.validStatusUpdates++;
// }

void Slave::statusUpdate(const StatusUpdate& update)
{
  const TaskStatus& status = update.status();

  LOG(INFO) << "Status update: task " << status.task_id()
            << " of framework " << update.framework_id()
            << " is now in state " << status.state();

  Framework* framework = getFramework(update.framework_id());
  if (framework != NULL) {
    Executor* executor = framework->getExecutor(status.task_id());
    if (executor != NULL) {
      executor->updateTaskState(status.task_id(), status.state());

      // Handle the task appropriately if it's terminated.
      if (status.state() == TASK_FINISHED ||
          status.state() == TASK_FAILED ||
          status.state() == TASK_KILLED ||
          status.state() == TASK_LOST) {
        executor->removeTask(status.task_id());

        dispatch(isolationModule,
                 &IsolationModule::resourcesChanged,
                 framework->id, executor->id, executor->resources);
      }

      // Send message and record the status for possible resending.
      StatusUpdateMessage message;
      message.mutable_update()->MergeFrom(update);
      message.set_reliable(true);
      send(master, message);

      // Send us a message to try and resend after some delay.
      delay(STATUS_UPDATE_RETRY_INTERVAL,
            self(), &Slave::statusUpdateTimeout, update);

      framework->updates[status.task_id()] = update;

      stats.tasks[status.state()]++;

      stats.validStatusUpdates++;
    } else {
      LOG(WARNING) << "Status update error: couldn't lookup "
                   << "executor for framework " << update.framework_id();
      stats.invalidStatusUpdates++;
    }
  } else {
    LOG(WARNING) << "Status update error: couldn't lookup "
                 << "framework " << update.framework_id();
    stats.invalidStatusUpdates++;
  }
}


void Slave::executorMessage(const SlaveID& slaveId,
                            const FrameworkID& frameworkId,
                            const ExecutorID& executorId,
                            const string& data)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Cannot send framework message from slave "
                 << slaveId << " to framework " << frameworkId
                 << " because framework does not exist";
    stats.invalidFrameworkMessages++;
    return;
  }

  LOG(INFO) << "Sending message for framework " << frameworkId
            << " to " << framework->pid;

  ExecutorToFrameworkMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);
  send(framework->pid, message);

  stats.validFrameworkMessages++;
}


void Slave::ping()
{
  send(from(), "PONG");
}


void Slave::statusUpdateTimeout(const StatusUpdate& update)
{
  // Check and see if we still need to send this update.
  Framework* framework = getFramework(update.framework_id());
  if (framework != NULL) {
    if (framework->updates.contains(update.status().task_id())) {
      // TODO(benh): This is not sufficient, need to check sequence!
      LOG(INFO) << "Resending status update"
                << " for task " << update.status().task_id()
                << " of framework " << update.framework_id();

      StatusUpdateMessage message;
      message.mutable_update()->MergeFrom(update);
      message.set_reliable(true);
      send(master, message);
    }
  }
}


// void Slave::timeout()
// {
//   // Check and see if we should re-send any status updates.
//   double now = elapsedTime();

//   foreachvalue (StatusUpdateStream* stream, statusUpdateStreams) {
//     CHECK(stream->timeout > 0);
//     if (stream->timeout < now) {
//       CHECK(!stream->pending.empty());
//       const StatusUpdate& update = stream->pending.front();

//       LOG(WARNING) << "WARNING! Resending status update"
//                 << " for task " << update.status().task_id()
//                 << " of framework " << update.framework_id();
      
//       StatusUpdateMessage message;
//       message.mutable_update()->MergeFrom(update);
//       message.set_reliable(true);
//       send(master, message);

//       stream->timeout = now + STATUS_UPDATE_RETRY_INTERVAL;
//     }
//   }
// }


void Slave::exited()
{
  LOG(INFO) << "Process exited: " << from();

  if (from() == master) {
    LOG(WARNING) << "WARNING! Master disconnected!"
                 << " Waiting for a new master to be elected.";
    // TODO(benh): After so long waiting for a master, commit suicide.
  }
}


Promise<HttpResponse> Slave::http_info_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/info.json'";

  std::ostringstream out;

  out <<
    "{" <<
    "\"built_date\":\"" << build::DATE << "\"," <<
    "\"build_user\":\"" << build::USER << "\"," <<
    "\"start_time\":\"" << startTime << "\"," <<
    "\"pid\":\"" << self() << "\"" <<
    "}";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_frameworks_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/frameworks.json'";

  std::ostringstream out;

  out << "[";

  foreachvalue (Framework* framework, frameworks) {
    out <<
      "{" <<
      "\"id\":\"" << framework->id << "\"," <<
      "\"name\":\"" << framework->info.name() << "\"," <<
      "\"user\":\"" << framework->info.user() << "\""
      "},";
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (frameworks.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_tasks_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/tasks.json'";

  std::ostringstream out;

  out << "[";

  foreachvalue (Framework* framework, frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      foreachvalue (Task* task, executor->launchedTasks) {
        // TODO(benh): Send all of the resources (as JSON).
        Resources resources(task->resources());
        Resource::Scalar cpus = resources.getScalar("cpus", Resource::Scalar());
        Resource::Scalar mem = resources.getScalar("mem", Resource::Scalar());
        out <<
          "{" <<
          "\"task_id\":\"" << task->task_id() << "\"," <<
          "\"framework_id\":\"" << task->framework_id() << "\"," <<
          "\"slave_id\":\"" << task->slave_id() << "\"," <<
          "\"name\":\"" << task->name() << "\"," <<
          "\"state\":\"" << task->state() << "\"," <<
          "\"cpus\":" << cpus.value() << "," <<
          "\"mem\":" << mem.value() <<
          "},";
      }
    }
  }

  // Backup the put pointer to overwrite the last comma (hack).
  if (frameworks.size() > 0) {
    long pos = out.tellp();
    out.seekp(pos - 1);
  }

  out << "]";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_stats_json(const HttpRequest& request)
{
  LOG(INFO) << "Http request for '/slave/stats.json'";

  std::ostringstream out;

  out <<
    "{" <<
    "\"uptime\":" << elapsedTime() - startTime << "," <<
    "\"total_frameworks\":" << frameworks.size() << "," <<
    "\"started_tasks\":" << stats.tasks[TASK_STARTING] << "," <<
    "\"finished_tasks\":" << stats.tasks[TASK_FINISHED] << "," <<
    "\"killed_tasks\":" << stats.tasks[TASK_KILLED] << "," <<
    "\"failed_tasks\":" << stats.tasks[TASK_FAILED] << "," <<
    "\"lost_tasks\":" << stats.tasks[TASK_LOST] << "," <<
    "\"valid_status_updates\":" << stats.validStatusUpdates << "," <<
    "\"invalid_status_updates\":" << stats.invalidStatusUpdates << "," <<
    "\"valid_framework_messages\":" << stats.validFrameworkMessages << "," <<
    "\"invalid_framework_messages\":" << stats.invalidFrameworkMessages <<
    "}";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/x-json;charset=UTF-8";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Promise<HttpResponse> Slave::http_vars(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/vars'";

  std::ostringstream out;

  out <<
    "build_date " << build::DATE << "\n" <<
    "build_user " << build::USER << "\n" <<
    "build_flags " << build::FLAGS << "\n";

  // Also add the configuration values.
  foreachpair (const string& key, const string& value, conf.getMap()) {
    out << key << " " << value << "\n";
  }

  out <<
    "uptime " << elapsedTime() - startTime << "\n" <<
    "total_frameworks " << frameworks.size() << "\n" <<
    "started_tasks " << stats.tasks[TASK_STARTING] << "\n" <<
    "finished_tasks " << stats.tasks[TASK_FINISHED] << "\n" <<
    "killed_tasks " << stats.tasks[TASK_KILLED] << "\n" <<
    "failed_tasks " << stats.tasks[TASK_FAILED] << "\n" <<
    "lost_tasks " << stats.tasks[TASK_LOST] << "\n" <<
    "valid_status_updates " << stats.validStatusUpdates << "\n" <<
    "invalid_status_updates " << stats.invalidStatusUpdates << "\n" <<
    "valid_framework_messages " << stats.validFrameworkMessages << "\n" <<
    "invalid_framework_messages " << stats.invalidFrameworkMessages << "\n";

  HttpOKResponse response;
  response.headers["Content-Type"] = "text/plain";
  response.headers["Content-Length"] = utils::stringify(out.str().size());
  response.body = out.str().data();
  return response;
}


Framework* Slave::getFramework(const FrameworkID& frameworkId)
{
  if (frameworks.count(frameworkId) > 0) {
    return frameworks[frameworkId];
  }

  return NULL;
}


// StatusUpdates* Slave::getStatusUpdateStream(const StatusUpdateStreamID& id)
// {
//   if (statusUpdateStreams.contains(id)) {
//     return statusUpdateStreams[id];
//   }

//   return NULL;
// }


// StatusUpdateStream* Slave::createStatusUpdateStream(
//     const FrameworkID& frameworkId,
//     const TaskID& taskId,
//     const string& directory)
// {
//   StatusUpdateStream* stream = new StatusUpdates();
//   stream->id = id;
//   stream->directory = directory;
//   stream->received = NULL;
//   stream->acknowledged = NULL;
//   stream->timeout = -1;

//   streams[id] = stream;

//   // Open file descriptors for "updates" and "acknowledged".
//   string path;
//   Result<int> result;

//   path = stream->directory + "/received";
//   result = utils::os::open(path, O_CREAT | O_RDWR | O_SYNC);
//   if (result.isError() || result.isNone()) {
//     LOG(WARNING) << "Failed to open " << path
//                  << " for storing received status updates";
//     cleanupStatusUpdateStream(stream);
//     return NULL;
//   }

//   stream->received = result.get();

//   path = updates->directory + "/acknowledged";
//   result = utils::os::open(path, O_CREAT | O_RDWR | O_SYNC);
//   if (result.isError() || result.isNone()) {
//     LOG(WARNING) << "Failed to open " << path << 
//                  << " for storing acknowledged status updates";
//     cleanupStatusUpdateStream(stream);
//     return NULL;
//   }

//   stream->acknowledged = result.get();

//   // Replay the status updates. This is necessary because the slave
//   // might have crashed but was restarted before the executors
//   // died. Or another task with the same id as before got run again on
//   // the same executor.
//   bool replayed = replayStatusUpdateStream(stream);

//   if (!replayed) {
//     LOG(WARNING) << "Failed to correctly replay status updates"
//                  << " for task " << taskId
//                  << " of framework " << frameworkId
//                  << " found at " << path;
//     cleanupStatusUpdateStream(stream);
//     return NULL;
//   }

//   // Start sending any pending status updates. In this case, the slave
//   // probably died after it sent the status update and never received
//   // the acknowledgement.
//   if (!stream->pending.empty()) {
//     StatusUpdate* update = stream->pending.front();
//     StatusUpdateMessage message;
//     message.mutable_update()->MergeFrom(*update);
//     message.set_reliable(true);
//     send(master, message);

//     stream->timeout = elapsedTime() + STATUS_UPDATE_RETRY_INTERVAL;
//   }

//   return stream;
// }


// bool Slave::replayStatusUpdateStream(StatusUpdateStream* stream)
// {
//   CHECK(stream->received != NULL);
//   CHECK(stream->acknowledged != NULL);

//   Result<StatusUpdate*> result;

//   // Okay, now read all the recevied status updates.
//   hashmap<uint32_t, StatusUpdate> pending;

//   result = utils::protobuf::read(stream->received);
//   while (result.isSome()) {
//     StatusUpdate* update = result.get();
//     CHECK(!pending.contains(update->sequence()));
//     pending[update->sequence()] = *update;
//     delete update;
//     result = utils::protobuf::read(stream->received);
//   }

//   if (result.isError()) {
//     return false;
//   }

//   CHECK(result.isNone());

//   LOG(INFO) << "Recovered " << pending.size()
//             << " TOTAL status updates for task "
//             << stream->id.second << " of framework "
//             << stream->id.first;

//   // Okay, now get all the acknowledged status updates.
//   result = utils::protobuf::read(stream->acknowledged);
//   while (result.isSome()) {
//     StatusUpdate* update = result.get();
//     stream->sequence = std::max(stream->sequence, update->sequence());
//     CHECK(pending.contains(update->sequence()));
//     pending.erase(update->sequence());
//     delete update;
//     result = utils::protobuf::read(stream->acknowledged);
//   }

//   if (result.isError()) {
//     return false;
//   }

//   CHECK(result.isNone());

//   LOG(INFO) << "Recovered " << pending.size()
//             << " PENDING status updates for task "
//             << stream->id.second << " of framework "
//             << stream->id.first;

//   // Add the pending status updates in sorted order.
//   uint32_t sequence = 0;

//   while (!pending.empty()) {
//     // Find the smallest sequence number.
//     foreachvalue (const StatusUpdate& update, pending) {
//       sequence = std::min(sequence, update.sequence());
//     }

//     // Push that update and remove it from pending.
//     stream->pending.push(pending[sequence]);
//     pending.erase(sequence);
//   }

//   return true;
// }


// void Slave::cleanupStatusUpdateStream(StatusUpdateStream* stream)
// {
//   if (stream->received != NULL) {
//     fclose(stream->received);
//   }

//   if (stream->acknowledged != NULL) {
//     fclose(stream->acknowledged);
//   }

//   streams.erase(stream->id);

//   delete stream;
// }


void Slave::executorStarted(const FrameworkID& frameworkId,
                            const ExecutorID& executorId,
                            pid_t pid)
{
  // TODO(benh): If the slave is running in "local" mode than the pid
  // is uninteresting here, and if we ever write the pid to file, we
  // should write something that makes is such that we don't try and
  // ever recover and connect to an executor with pid 0!
}


// Called by the isolation module when an executor process exits.
void Slave::executorExited(const FrameworkID& frameworkId,
                           const ExecutorID& executorId,
                           int status)
{
  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "WARNING! Unknown executor '" << executorId
                 << "' of unknown framework " << frameworkId
                 << " has exited with status " << status;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "UNKNOWN executor '" << executorId
                 << "' of framework " << frameworkId
                 << " has exited with status " << status;
    return;
  }

  LOG(INFO) << "Exited executor '" << executorId
            << "' of framework " << frameworkId
            << " with status " << status;

  ExitedExecutorMessage message;
  message.mutable_slave_id()->MergeFrom(id);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_status(status);
  send(master, message);

  removeExecutor(framework, executor, false);

  if (framework->executors.size() == 0) {
    removeFramework(framework);
  }
}


// Remove a framework (including its executor(s) if killExecutors is true).
void Slave::removeFramework(Framework* framework, bool killExecutors)
{
  LOG(INFO) << "Cleaning up framework " << framework->id;

  // Shutdown all executors of this framework.
  foreachvalue (Executor* executor, utils::copy(framework->executors)) {
    removeExecutor(framework, executor, killExecutors);
  }

  frameworks.erase(framework->id);
  delete framework;
}


void Slave::removeExecutor(Framework* framework,
                           Executor* executor,
                           bool killExecutor)
{
  if (killExecutor) {
    LOG(INFO) << "Shutting down executor '" << executor->id
              << "' of framework " << framework->id;

    send(executor->pid, ShutdownMessage());

    // TODO(benh): There really isn't ANY time between when an
    // executor gets a shutdown message and the isolation module goes
    // and kills it. We should really think about making the semantics
    // of this better.

    LOG(INFO) << "Killing executor '" << executor->id
              << "' of framework " << framework->id;

    dispatch(isolationModule,
             &IsolationModule::killExecutor,
             framework->id, executor->id);
  }

  // TODO(benh): We need to push a bunch of status updates which
  // signifies all tasks are dead (once the Master stops doing this
  // for us).

  framework->destroyExecutor(executor->id);
}


// void Slave::recover()
// {
//   // if we find an executor that is no longer running and it's last
//   // acknowledged task statuses are not terminal, create a
//   // statusupdatestream for each task and try and reliably send
//   // TASK_LOST updates.

//   // otherwise once we reconnect the executor will just start sending
//   // us status updates that we need to send, wait for ack, write to
//   // disk, and then respond.
// }


string Slave::getUniqueWorkDirectory(const FrameworkID& frameworkId,
                                     const ExecutorID& executorId)
{
  LOG(INFO) << "Generating a unique work directory for executor '"
            << executorId << "' of framework " << frameworkId;

  string workDir = ".";
  if (conf.contains("work_dir")) {
    workDir = conf.get("work_dir", workDir);
  } else if (conf.contains("home")) {
    workDir = conf.get("home", workDir);
  }

  workDir = workDir + "/work";

  std::ostringstream out(std::ios_base::app | std::ios_base::out);
  out << workDir << "/slave-" << id
     << "/fw-" << frameworkId << "-" << executorId;

  // TODO(benh): Make executor id be in it's own directory.

  // Find a unique directory based on the path given by the slave
  // (this is because we might launch multiple executors from the same
  // framework on this slave).
  out << "/";

  string dir;
  dir = out.str();

  for (int i = 0; i < INT_MAX; i++) {
    out << i;
    if (opendir(out.str().c_str()) == NULL && errno == ENOENT)
      break;

    // TODO(benh): Does one need to do any sort of closedir?

    out.str(dir);
  }

  return out.str();
}


}}} // namespace mesos { namespace internal { namespace slave {
