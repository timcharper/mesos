#ifndef __SLAVE_HPP__
#define __SLAVE_HPP__

#include <process/process.hpp>

#include "isolation_module.hpp"
#include "state.hpp"

#include "common/resources.hpp"
#include "common/hashmap.hpp"

#include "configurator/configurator.hpp"

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace slave {

struct Framework;
struct Executor;
struct ExecutorReaper;


const double STATUS_UPDATE_RETRY_INTERVAL = 10;


// Slave process. 
class Slave : public MesosProcess<Slave>
{
public:
  Slave(const Configuration& conf,
        bool local,
        IsolationModule *isolationModule);

  Slave(const Resources& resources,
        bool local,
        IsolationModule* isolationModule);

  virtual ~Slave();

  static void registerOptions(Configurator* configurator);

  process::Promise<state::SlaveState*> getState();

  void newMasterDetected(const std::string& pid);
  void noMasterDetected();
  void masterDetectionFailure();
  void registerReply(const SlaveID& slaveId);
  void reregisterReply(const SlaveID& slaveId);
  void runTask(const FrameworkInfo& frameworkInfo,
               const FrameworkID& frameworkId,
               const std::string& pid,
               const TaskDescription& task);
  void killTask(const FrameworkID& frameworkId,
                const TaskID& taskId);
  void killFramework(const FrameworkID& frameworkId);
  void schedulerMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const std::string& data);
  void updateFramework(const FrameworkID& frameworkId,
                       const std::string& pid);
  void statusUpdateAcknowledged(const SlaveID& slaveId,
                                const FrameworkID& frameworkId,
                                const TaskID& taskId);
  void registerExecutor(const FrameworkID& frameworkId,
                        const ExecutorID& executorId);
  void statusUpdate(const StatusUpdate& update);
  void executorMessage(const SlaveID& slaveId,
                       const FrameworkID& frameworkId,
                       const ExecutorID& executorId,
                       const std::string& data);
  void ping();

  void timeout();
  void exited();

  // TODO(benh): Have the isolation module actually call:
  // void executorStarted(const FrameworkID& frameworkId,
  //                      const ExecutorID& executorId,
  //                      pid_t pid);

  // Callback used by reaper to tell us when an executor exits.
  void executorExited(const FrameworkID& frameworkId,
                      const ExecutorID& executorId,
                      int result);

protected:
  virtual void operator () ();

  void initialize();

  // Helper routine to lookup a framework.
  Framework* getFramework(const FrameworkID& frameworkId);

  // Remove a framework (possibly killing its executors).
  void removeFramework(Framework* framework, bool killExecutors = true);

  // Remove an executor (possibly sending it a kill).
  void removeExecutor(Framework* framework,
                      Executor* executor,
                      bool killExecutor = true);

//   // Create a new status update stream.
//   StatusUpdates* createStatusUpdateStream(const StatusUpdateStreamID& streamId,
//                                           const string& directory);

//   StatusUpdates* getStatusUpdateStream(const StatusUpdateStreamID& streamId);

  // Helper function for generating a unique work directory for this
  // framework/executor pair (non-trivial since a framework/executor
  // pair may be launched more than once on the same slave).
  std::string getUniqueWorkDirectory(const FrameworkID& frameworkId,
                                     const ExecutorID& executorId);

private:
  // TODO(benh): Better naming and name scope for these http handlers.
  process::Promise<process::HttpResponse> http_info_json(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> http_frameworks_json(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> http_tasks_json(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> http_stats_json(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> http_vars(const process::HttpRequest& request);

  const Configuration conf;

  bool local;

  SlaveID id;
  SlaveInfo info;

  process::UPID master;

  Resources resources;

  hashmap<FrameworkID, Framework*> frameworks;

  ExecutorReaper* reaper;

  IsolationModule* isolationModule;

  // Statistics (initialized in Slave::initialize).
  struct {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } statistics;

  double startTime;

//   typedef std::pair<FrameworkID, TaskID> StatusUpdateStreamID;
//   hashmap<std::pair<FrameworkID, TaskID>, StatusUpdateStream*> statusUpdateStreams;

//   hashmap<std::pair<FrameworkID, TaskID>, PendingStatusUpdate> pendingUpdates;
};

}}}

#endif /* __SLAVE_HPP__ */
