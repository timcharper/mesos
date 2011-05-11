#include <errno.h>
#include <netdb.h>

#include <algorithm>

#include "slave.hpp"

#include "common/build.hpp"
#include "common/type_utils.hpp"
#include "common/utils.hpp"

// There's no gethostbyname2 on Solaris, so fake it by calling gethostbyname
#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using process::HttpOKResponse;
using process::HttpResponse;
using process::HttpRequest;
using process::PID;
using process::Process;
using process::Promise;
using process::UPID;

using std::make_pair;
using std::ostringstream;
using std::pair;
using std::string;
using std::queue;


namespace mesos { namespace internal { namespace slave {

// Information describing an executor (goes away if executor crashes).
struct Executor
{
  Executor(const FrameworkID& _frameworkId,
           const ExecutorInfo& _info,
           const std::string& _directory)
    : frameworkId(_frameworkId),
      info(_info),
      directory(_directory),
      id(info.executor_id()),
      pid(process::UPID()) {}

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

  const std::string directory;

  process::UPID pid;

  Resources resources;

  hashmap<TaskID, TaskDescription> queuedTasks;
  hashmap<TaskID, Task*> launchedTasks;
};


// Information about a framework.
struct Framework
{
  Framework(const FrameworkID& _id,
            const FrameworkInfo& _info,
            const process::UPID& _pid)
    : id(_id), info(_info), pid(_pid) {}

  virtual ~Framework() {}

  Executor* createExecutor(const ExecutorInfo& info,
                           const std::string& directory)
  {
    Executor* executor = new Executor(id, info, directory);
    CHECK(!executors.contains(info.executor_id()));
    executors[info.executor_id()] = executor;
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

  process::UPID pid;

  hashmap<ExecutorID, Executor*> executors;
  hashmap<double, hashmap<TaskID, StatusUpdate> > updates;
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


class ExecutorReaper : public Process<ExecutorReaper>
{
public:
  ExecutorReaper(const PID<Slave>& _slave)
    : Process<ExecutorReaper>("reaper"), slave(_slave) {}

  void watch(const FrameworkID& frameworkId,
             const ExecutorID& executorId,
             pid_t pid)
  {
    if (!exited.contains(pid)) {
      LOG(INFO) << "Reaper watching for process " << pid << " to exit";
      watching[pid] = make_pair(frameworkId, executorId);
    } else {
      LOG(INFO) << "Telling slave of exited executor '" << executorId
                << "' of framework " << frameworkId;
      process::dispatch(slave, &Slave::executorExited,
                        frameworkId, executorId, exited[pid]);
      exited.erase(pid);
    }
  }

protected:
  virtual void operator () ()
  {
    link(slave);
    while (true) {
      serve(1);
      if (name() == process::TIMEOUT) {
        // Check whether any child process has exited.
        pid_t pid;
        int status;
        if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
          LOG(INFO) << "Reaper reaping exited process " << pid
                    << " with status " << status;
          if (watching.contains(pid)) {
            const FrameworkID& frameworkId = watching[pid].first;
            const ExecutorID& executorId = watching[pid].second;

            LOG(INFO) << "Telling slave of exited executor '" << executorId
                      << "' of framework " << frameworkId;
            process::dispatch(slave, &Slave::executorExited,
                              frameworkId, executorId, status);

            watching.erase(pid);
          } else {
            exited[pid] = status;
          }
        }
      } else if (name() == process::TERMINATE || name() == process::EXITED) {
        LOG(WARNING) << "WARNING! Executor reaper is exiting ...";
        return;
      }
    }
  }

private:
  const PID<Slave> slave;

  hashmap<pid_t, pair<FrameworkID, ExecutorID> > watching;
  hashmap<pid_t, int> exited;
};


Slave::Slave(const Configuration& _conf,
             bool _local,
             IsolationModule* _isolationModule)
  : MesosProcess<Slave>("slave"),
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
  : MesosProcess<Slave>("slave"),
    resources(_resources),
    local(_local),
    isolationModule(_isolationModule)
{
  initialize();
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


Slave::~Slave()
{
  // TODO(benh): Shut down and free frameworks?

  // TODO(benh): Shut down and free executors? The executor should get
  // an "exited" event and initiate shutdown itself.
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
  // Startup the executor reaper.
  reaper = new ExecutorReaper(self());
  process::spawn(reaper, true);

  // Start all the statistics at 0.
  CHECK(TASK_STARTING == TaskState_MIN);
  CHECK(TASK_LOST == TaskState_MAX);
  statistics.tasks[TASK_STARTING] = 0;
  statistics.tasks[TASK_RUNNING] = 0;
  statistics.tasks[TASK_FINISHED] = 0;
  statistics.tasks[TASK_FAILED] = 0;
  statistics.tasks[TASK_KILLED] = 0;
  statistics.tasks[TASK_LOST] = 0;
  statistics.validStatusUpdates = 0;
  statistics.invalidStatusUpdates = 0;
  statistics.validFrameworkMessages = 0;
  statistics.invalidFrameworkMessages = 0;

  startTime = elapsedTime();

  // Install protobuf handlers.
  install(NEW_MASTER_DETECTED, &Slave::newMasterDetected,
          &NewMasterDetectedMessage::pid);

  install(NO_MASTER_DETECTED, &Slave::noMasterDetected);

  install(M2S_REGISTER_REPLY, &Slave::registerReply,
          &SlaveRegisteredMessage::slave_id);

  install(M2S_REREGISTER_REPLY, &Slave::reregisterReply,
          &SlaveRegisteredMessage::slave_id);

  install(M2S_RUN_TASK, &Slave::runTask,
          &RunTaskMessage::framework,
          &RunTaskMessage::framework_id,
          &RunTaskMessage::pid,
          &RunTaskMessage::task);

  install(M2S_KILL_TASK, &Slave::killTask,
          &KillTaskMessage::framework_id,
          &KillTaskMessage::task_id);

  install(M2S_KILL_FRAMEWORK, &Slave::killFramework,
          &KillFrameworkMessage::framework_id);

  install(M2S_FRAMEWORK_MESSAGE, &Slave::schedulerMessage,
          &FrameworkMessageMessage::slave_id,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::executor_id,
          &FrameworkMessageMessage::data);

  install(M2S_UPDATE_FRAMEWORK, &Slave::updateFramework,
          &UpdateFrameworkMessage::framework_id,
          &UpdateFrameworkMessage::pid);

  install(M2S_STATUS_UPDATE_ACK, &Slave::statusUpdateAcknowledged,
          &StatusUpdateAcknowledgedMessage::slave_id,
          &StatusUpdateAcknowledgedMessage::framework_id,
          &StatusUpdateAcknowledgedMessage::task_id);

  install(E2S_REGISTER_EXECUTOR, &Slave::registerExecutor,
          &RegisterExecutorMessage::framework_id,
          &RegisterExecutorMessage::executor_id);

  install(E2S_STATUS_UPDATE, &Slave::statusUpdate,
          &StatusUpdateMessage::update);

  install(E2S_FRAMEWORK_MESSAGE, &Slave::executorMessage,
          &FrameworkMessageMessage::slave_id,
          &FrameworkMessageMessage::framework_id,
          &FrameworkMessageMessage::executor_id,
          &FrameworkMessageMessage::data);

  install(PING, &Slave::ping);

  // Install some message handlers.
  installMessageHandler(process::TIMEOUT, &Slave::timeout);
  installMessageHandler(process::EXITED, &Slave::exited);

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

  // Get our hostname
  char buf[512];
  gethostname(buf, sizeof(buf));
  hostent* he = gethostbyname2(buf, AF_INET);
  string hostname = he->h_name;

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

  // Initialize isolation module.
  isolationModule->initialize(self(), conf, local);

  while (true) {
    serve(1);
    if (name() == process::TERMINATE) {
      LOG(INFO) << "Asked to shut down by " << from();
      foreachvaluecopy (Framework* framework, frameworks) {
        removeFramework(framework);
      }
      return;
    }
  }
}


void Slave::newMasterDetected(const string& pid)
{
  LOG(INFO) << "New master detected at " << pid;

  master = pid;
  link(master);

  if (id == "") {
    // Slave started before master.
    MSG<S2M_REGISTER_SLAVE> out;
    out.mutable_slave()->MergeFrom(info);
    send(master, out);
  } else {
    // Re-registering, so send tasks running.
    MSG<S2M_REREGISTER_SLAVE> out;
    out.mutable_slave_id()->MergeFrom(id);
    out.mutable_slave()->MergeFrom(info);

    foreachvalue (Framework* framework, frameworks) {
      foreachvalue (Executor* executor, framework->executors) {
	foreachvalue (Task* task, executor->launchedTasks) {
          // TODO(benh): Also need to send queued tasks here ...
	  out.add_tasks()->MergeFrom(*task);
	}
      }
    }

    send(master, out);
  }
}


void Slave::noMasterDetected()
{
  LOG(INFO) << "Lost master(s) ... waiting";
}


void Slave::registerReply(const SlaveID& slaveId)
{
  LOG(INFO) << "Registered with master; given slave ID " << slaveId;
  id = slaveId;
}


void Slave::reregisterReply(const SlaveID& slaveId)
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

      MSG<S2E_RUN_TASK> out;
      out.mutable_framework()->MergeFrom(framework->info);
      out.mutable_framework_id()->MergeFrom(framework->id);
      out.set_pid(framework->pid);
      out.mutable_task()->MergeFrom(task);
      send(executor->pid, out);

      // Now update the resources.
      isolationModule->resourcesChanged(
          framework->id, framework->info,
          executor->info, executor->resources);
    }
  } else {
    // Launch an executor for this task.
    const string& directory =
      getUniqueWorkDirectory(framework->id, executor->id);

    if (task.has_executor()) {
      executor =
        framework->createExecutor(task.executor(), directory);
    } else {
      executor =
        framework->createExecutor(framework->info.executor(), directory);
    }

    // Queue task until the executor starts up.
    executor->queuedTasks[task.task_id()] = task;

    // Tell the isolation module to launch the executor. (TODO(benh):
    // Make the isolation module a process so that it can block while
    // trying to launch the executor. Also, have the isolation module
    // ultimately dispatch to Slave::executorStarted rather than
    // returning here.)
    pid_t pid = isolationModule->launchExecutor(
        framework->id, framework->info,
        executor->info, directory);

    // For now, an isolation module returning 0 effectively indicates
    // that the slave shouldn't try and reap it to determine if it has
    // exited, but instead that will be done another way.

    // TODO(benh): Put the reaper in it's own file and return to the
    // isolation module starting/stopping it, that way it can be used
    // by both the lxc isolation module and the process based
    // isolation module without duplicate code.

    // Tell the executor reaper to monitor/reap this process.
    if (pid != 0) {
      process::dispatch(reaper->self(), &ExecutorReaper::watch,
                        framework->id, executor->id, pid);
    }
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

    MSG<S2M_STATUS_UPDATE> out;
    StatusUpdate* update = out.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus *status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_LOST);
    update->set_timestamp(elapsedTime());
    update->set_sequence(-1);
    out.set_reliable(false);
    send(master, out);

    return;
  }


  // Tell the executor to kill the task if it is up and
  // running, otherwise, consider the task lost.
  Executor* executor = framework->getExecutor(taskId);
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Cannot kill task " << taskId
                 << " of framework " << frameworkId
                 << " because no such task is running";

    MSG<S2M_STATUS_UPDATE> out;
    StatusUpdate* update = out.mutable_update();
    update->mutable_framework_id()->MergeFrom(framework->id);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus *status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_LOST);
    update->set_timestamp(elapsedTime());
    update->set_sequence(-1);
    out.set_reliable(false);
    send(master, out);
  } else if (!executor->pid) {
    // Remove the task and update the resources.
    executor->removeTask(taskId);

    isolationModule->resourcesChanged(
        framework->id, framework->info,
        executor->info, executor->resources);

    MSG<S2M_STATUS_UPDATE> out;
    StatusUpdate* update = out.mutable_update();
    update->mutable_framework_id()->MergeFrom(framework->id);
    update->mutable_executor_id()->MergeFrom(executor->id);
    update->mutable_slave_id()->MergeFrom(id);
    TaskStatus *status = update->mutable_status();
    status->mutable_task_id()->MergeFrom(taskId);
    status->set_state(TASK_KILLED);
    update->set_timestamp(elapsedTime());
    update->set_sequence(0);
    out.set_reliable(false);
    send(master, out);
  } else {
    // Otherwise, send a message to the executor and wait for
    // it to send us a status update.
    MSG<S2E_KILL_TASK> out;
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_task_id()->MergeFrom(taskId);
    send(executor->pid, out);
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
    statistics.invalidFrameworkMessages++;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor does not exist";
    statistics.invalidFrameworkMessages++;
  } else if (!executor->pid) {
    // TODO(*): If executor is not started, queue framework message?
    // (It's probably okay to just drop it since frameworks can have
    // the executor send a message to the master to say when it's ready.)
    LOG(WARNING) << "Dropping message for executor '"
                 << executorId << "' of framework " << frameworkId
                 << " because executor is not running";
    statistics.invalidFrameworkMessages++;
  } else {
    MSG<S2E_FRAMEWORK_MESSAGE> out;
    out.mutable_slave_id()->MergeFrom(slaveId);
    out.mutable_framework_id()->MergeFrom(frameworkId);
    out.mutable_executor_id()->MergeFrom(executorId);
    out.set_data(data);
    send(executor->pid, out);

    statistics.validFrameworkMessages++;
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


void Slave::statusUpdateAcknowledged(const SlaveID& slaveId,
                                     const FrameworkID& frameworkId,
                                     const TaskID& taskId)
{
  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    foreachkey (double deadline, framework->updates) {
      if (framework->updates[deadline].contains(taskId)) {
        LOG(INFO) << "Got acknowledgement of status update"
                  << " for task " << taskId
                  << " of framework " << framework->id;
        framework->updates[deadline].erase(taskId);
        break;
      }
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
//       MSG<S2M_STATUS_UPDATE> out;
//       out.mutable_update()->MergeFrom(stream->pending.front());
//       out.set_reliable(true);
//       send(master, out);

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

    // TODO(benh): Should we be sending a TERMINATE instead?
    send(from(), S2E_KILL_EXECUTOR);

    return;
  }

  Executor* executor = framework->getExecutor(executorId);

  // Check the status of the executor.
  if (executor == NULL) {
    LOG(WARNING) << "WARNING! Unexpected executor '" << executorId
                 << "' registering for framework " << frameworkId;
    // TODO(benh): Should we be sending a TERMINATE instead?
    send(from(), S2E_KILL_EXECUTOR);
  } else if (executor->pid != UPID()) {
    LOG(WARNING) << "WARNING! executor '" << executorId
                 << "' of framework " << frameworkId
                 << " is already running";
    // TODO(benh): Should we be sending a TERMINATE instead?
    send(from(), S2E_KILL_EXECUTOR);
  } else {
    // Save the pid for the executor.
    executor->pid = from();

    // Now that the executor is up, set its resource limits.
    isolationModule->resourcesChanged(
        framework->id, framework->info,
        executor->info, executor->resources);

    // Tell executor it's registered and give it any queued tasks.
    MSG<S2E_REGISTER_REPLY> out;
    ExecutorArgs* args = out.mutable_args();
    args->mutable_framework_id()->MergeFrom(framework->id);
    args->mutable_executor_id()->MergeFrom(executor->id);
    args->mutable_slave_id()->MergeFrom(id);
    args->set_hostname(info.hostname());
    args->set_data(executor->info.data());
    send(executor->pid, out);

    LOG(INFO) << "Flushing queued tasks for framework " << framework->id;

    foreachvalue (const TaskDescription& task, executor->queuedTasks) {
      // Add the task to the executor.
      executor->addTask(task);

      MSG<S2E_RUN_TASK> out;
      out.mutable_framework_id()->MergeFrom(framework->id);
      out.mutable_framework()->MergeFrom(framework->info);
      out.set_pid(framework->pid);
      out.mutable_task()->MergeFrom(task);
      send(executor->pid, out);
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
//     statistics.invalidStatusUpdates++;
//     return;
//   }

//   Executor* executor = framework->getExecutor(update.status().task_id());
//   if (executor == NULL) {
//     LOG(WARNING) << "WARNING! Failed to lookup executor"
//                  << " for framework " << update.framework_id()
//                  << " of received status update";
//     statistics.invalidStatusUpdates++;
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
//   MSG<S2E_STATUS_UPDATE_ACK> out;
//   out.mutable_framework_id()->MergeFrom(update.framework_id());
//   out.mutable_slave_id()->MergeFrom(update.slave_id());
//   out.mutable_task_id()->MergeFrom(update.status().task_id());
//   send(executor->pid, out);

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
//     MSG<S2M_STATUS_UPDATE> out;
//     out.mutable_update()->MergeFrom(update);
//     out.set_reliable(true);
//     send(master, out);

//     stream->timeout = elapsedTime() + STATUS_UPDATE_RETRY_INTERVAL;
//   }

//   statistics.tasks[status.state()]++;
//   statistics.validStatusUpdates++;
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
      if (status.state() == TASK_FINISHED ||
          status.state() == TASK_FAILED ||
          status.state() == TASK_KILLED ||
          status.state() == TASK_LOST) {
        executor->removeTask(status.task_id());
        isolationModule->resourcesChanged(
            framework->id, framework->info,
            executor->info, executor->resources);
      }

      // Send message and record the status for possible resending.
      MSG<S2M_STATUS_UPDATE> out;
      out.mutable_update()->MergeFrom(update);
      out.set_reliable(true);
      send(master, out);

      double deadline = elapsedTime() + STATUS_UPDATE_RETRY_INTERVAL;
      framework->updates[deadline][status.task_id()] = update;
    } else {
      LOG(WARNING) << "Status update error: couldn't lookup "
                   << "executor for framework " << update.framework_id();
    }
  } else {
    LOG(WARNING) << "Status update error: couldn't lookup "
                 << "framework " << update.framework_id();
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
    statistics.invalidFrameworkMessages++;
    return;
  }

  LOG(INFO) << "Sending message for framework " << frameworkId
            << " to " << framework->pid;

  // TODO(benh): This is weird, sending an M2F message.
  MSG<M2F_FRAMEWORK_MESSAGE> out;
  out.mutable_slave_id()->MergeFrom(slaveId);
  out.mutable_framework_id()->MergeFrom(frameworkId);
  out.mutable_executor_id()->MergeFrom(executorId);
  out.set_data(data);
  send(framework->pid, out);

  statistics.validFrameworkMessages++;
}


void Slave::ping()
{
  send(from(), PONG);
}


void Slave::timeout()
{
  // Check and see if we should re-send any status updates.
  foreachvalue (Framework* framework, frameworks) {
    foreachkey (double deadline, framework->updates) {
      if (deadline <= elapsedTime()) {
        foreachvalue (const StatusUpdate& update, framework->updates[deadline]) {
          LOG(WARNING) << "Resending status update"
                       << " for task " << update.status().task_id()
                       << " of framework " << framework->id;
          MSG<S2M_STATUS_UPDATE> out;
          out.mutable_update()->MergeFrom(update);
          out.set_reliable(true);
          send(master, out);
        }
      }
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
      
//       MSG<S2M_STATUS_UPDATE> out;
//       out.mutable_update()->MergeFrom(update);
//       out.set_reliable(true);
//       send(master, out);

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
  } else if (from() == reaper->self()) {
    LOG(FATAL) << "Lost our executor reaper!";
  }
}


Promise<HttpResponse> Slave::http_info_json(const HttpRequest& request)
{
  LOG(INFO) << "HTTP request for '/slave/info.json'";

  ostringstream out;

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

  ostringstream out;

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

  ostringstream out;

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

  ostringstream out;

  out <<
    "{" <<
    "\"uptime\":" << elapsedTime() - startTime << "," <<
    "\"total_frameworks\":" << frameworks.size() << "," <<
    "\"finished_tasks\":" << statistics.tasks[TASK_FINISHED] << "," <<
    "\"killed_tasks\":" << statistics.tasks[TASK_KILLED] << "," <<
    "\"failed_tasks\":" << statistics.tasks[TASK_FAILED] << "," <<
    "\"lost_tasks\":" << statistics.tasks[TASK_LOST] << "," <<
    "\"valid_status_updates\":" << statistics.validStatusUpdates << "," <<
    "\"invalid_status_updates\":" << statistics.invalidStatusUpdates << "," <<
    "\"valid_framework_messages\":" << statistics.validFrameworkMessages << "," <<
    "\"invalid_framework_messages\":" << statistics.invalidFrameworkMessages <<
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

  ostringstream out;

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
    "finished_tasks " << statistics.tasks[TASK_FINISHED] << "\n" <<
    "killed_tasks " << statistics.tasks[TASK_KILLED] << "\n" <<
    "failed_tasks " << statistics.tasks[TASK_FAILED] << "\n" <<
    "lost_tasks " << statistics.tasks[TASK_LOST] << "\n" <<
    "valid_status_updates " << statistics.validStatusUpdates << "\n" <<
    "invalid_status_updates " << statistics.invalidStatusUpdates << "\n" <<
    "valid_framework_messages " << statistics.validFrameworkMessages << "\n" <<
    "invalid_framework_messages " << statistics.invalidFrameworkMessages << "\n";

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
//     MSG<S2M_STATUS_UPDATE> out;
//     out.mutable_update()->MergeFrom(*update);
//     out.set_reliable(true);
//     send(master, out);

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


// Called by the ExecutorReaper when an executor process exits.
void Slave::executorExited(const FrameworkID& frameworkId,
                           const ExecutorID& executorId,
                           int result)
{
  // TODO(benh): Two things: (1) We need to deal with the case that
  // will kill an executor, then we relaunch another executor with the
  // same executor id, then the reaper tells us that that executor id
  // has exited, because we will incorrectly kill that
  // executor. Ugh. (2) We need to get the remaining status updates
  // that have been stored after this executor exits. There is a TODO
  // in Slave::removeExecutor for this, but it should really be done
  // here because this is when we "know" the process has really
  // finished and no more updates will get written.

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "WARNING! Unknown executor '" << executorId
                 << "' of unknown framework " << frameworkId
                 << " has exited with result " << result;
    return;
  }

  Executor* executor = framework->getExecutor(executorId);
  if (executor == NULL) {
    LOG(WARNING) << "UNKNOWN executor '" << executorId
                 << "' of framework " << frameworkId
                 << " has exited with result " << result;
    return;
  }

  LOG(INFO) << "Exited executor '" << executorId
            << "' of framework " << frameworkId
            << " with result " << result;

  MSG<S2M_EXITED_EXECUTOR> out;
  out.mutable_slave_id()->MergeFrom(id);
  out.mutable_framework_id()->MergeFrom(frameworkId);
  out.mutable_executor_id()->MergeFrom(executorId);
  out.set_result(result);
  send(master, out);

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
  foreachvaluecopy (Executor* executor, framework->executors) {
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
    LOG(INFO) << "Killing executor '" << executor->id
              << "' of framework " << framework->id;

    send(executor->pid, S2E_KILL_EXECUTOR);

    // TODO(benh): There really isn't ANY time between when an
    // executor gets a S2E_KILL_EXECUTOR message and the isolation
    // module goes and kills it. We should really think about making
    // the semantics of this better.

    isolationModule->killExecutor(framework->id,
                                  framework->info,
                                  executor->info);
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
  string workDir = ".";
  if (conf.contains("work_dir")) {
    workDir = conf.get("work_dir", workDir);
  } else if (conf.contains("home")) {
    workDir = conf.get("home", workDir);
  }

  workDir = workDir + "/work";

  ostringstream os(std::ios_base::app | std::ios_base::out);
  os << workDir << "/slave-" << id
     << "/fw-" << frameworkId << "-" << executorId;

  // Find a unique directory based on the path given by the slave
  // (this is because we might launch multiple executors from the same
  // framework on this slave).
  os << "/";

  string dir;
  dir = os.str();

  for (int i = 0; i < INT_MAX; i++) {
    os << i;
    if (opendir(os.str().c_str()) == NULL && errno == ENOENT)
      break;
    os.str(dir);
  }

  return os.str();
}


}}} // namespace mesos { namespace internal { namespace slave {
