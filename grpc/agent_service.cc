// Copyright 2023 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "grpc/agent_service.h"

#include <string_view>
#include <vector>

#include <absl/log/check.h>
#include <absl/strings/str_format.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <mujoco/mujoco.h>
#include "grpc/agent.pb.h"
#include "grpc/grpc_agent_util.h"
#include "mjpc/task.h"

namespace agent_grpc {

using ::agent::GetActionRequest;
using ::agent::GetActionResponse;
using ::agent::GetCostValuesAndWeightsRequest;
using ::agent::GetCostValuesAndWeightsResponse;
using ::agent::GetStateRequest;
using ::agent::GetStateResponse;
using ::agent::GetTaskParametersRequest;
using ::agent::GetTaskParametersResponse;
using ::agent::InitRequest;
using ::agent::InitResponse;
using ::agent::PlannerStepRequest;
using ::agent::PlannerStepResponse;
using ::agent::ResetRequest;
using ::agent::ResetResponse;
using ::agent::SetCostWeightsRequest;
using ::agent::SetCostWeightsResponse;
using ::agent::SetModeRequest;
using ::agent::SetModeResponse;
using ::agent::GetModeRequest;
using ::agent::GetModeResponse;
using ::agent::SetStateRequest;
using ::agent::SetStateResponse;
using ::agent::SetTaskParametersRequest;
using ::agent::SetTaskParametersResponse;
using ::agent::StepRequest;
using ::agent::StepResponse;

// task used to define desired behaviour
mjpc::Task* task = nullptr;

// model used for physics
mjModel* model = nullptr;

// model used for planning, owned by the Agent instance.
mjModel* agent_model = nullptr;

void residual_sensor_callback(const mjModel* m, mjData* d, int stage) {
  // with the `m == model` guard in place, no need to clear the callback.
  if (m == agent_model || m == model) {
    if (stage == mjSTAGE_ACC) {
      task->Residual(m, d, d->sensordata);
    }
  }
}

grpc::Status AgentService::Init(grpc::ServerContext* context,
                                const InitRequest* request,
                                InitResponse* response) {
  agent_.SetTaskList(tasks_);
  grpc::Status status = grpc_agent_util::InitAgent(&agent_, request);
  if (!status.ok()) {
    return status;
  }
  agent_.SetTaskList(tasks_);
  std::string_view task_id = request->task_id();
  int task_index = agent_.GetTaskIdByName(task_id);
  if (task_index == -1) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        absl::StrFormat("Invalid task_id: '%s'", task_id));
  }
  agent_.SetTaskByIndex(task_index);

  auto load_model = agent_.LoadModel();
  if (!load_model.model) {
    return grpc::Status(
        grpc::StatusCode::INTERNAL,
        absl::StrCat("Failed to load model: ", load_model.error));
  }

  agent_.Initialize(load_model.model.get());
  agent_.Allocate();
  agent_.Reset();

  task = agent_.ActiveTask();
  CHECK_EQ(agent_model, nullptr)
      << "Multiple instances of AgentService detected.";
  agent_model = agent_.GetModel();
  // copy the model before agent model's timestep and integrator are updated
  CHECK_EQ(model, nullptr)
      << "Multiple instances of AgentService detected.";
  model = mj_copyModel(nullptr, agent_model);
  data_ = mj_makeData(model);
  mjcb_sensor = residual_sensor_callback;

  agent_.SetState(data_);

  agent_.plan_enabled = true;
  agent_.action_enabled = true;

  return grpc::Status::OK;
}

AgentService::~AgentService() {
  if (data_) mj_deleteData(data_);
  if (model) mj_deleteModel(model);
  model = nullptr;
  // no need to delete agent_model and task, since they're owned by agent_.
  agent_model = nullptr;
  task = nullptr;
  mjcb_sensor = nullptr;
}

grpc::Status AgentService::GetState(grpc::ServerContext* context,
                                    const GetStateRequest* request,
                                    GetStateResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  agent_.ActiveState().CopyTo(model, data_);
  return grpc_agent_util::GetState(model, data_, response);
}

grpc::Status AgentService::SetState(grpc::ServerContext* context,
                                    const SetStateRequest* request,
                                    SetStateResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  grpc::Status status =
      grpc_agent_util::SetState(request, &agent_, model, data_);
  if (!status.ok()) return status;

  mj_forward(model, data_);
  task->Transition(model, data_);

  return grpc::Status::OK;
}

grpc::Status AgentService::GetAction(grpc::ServerContext* context,
                                     const GetActionRequest* request,
                                     GetActionResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::GetAction(request, &agent_, model, data_, response);
}

grpc::Status AgentService::GetCostValuesAndWeights(
    grpc::ServerContext* context, const GetCostValuesAndWeightsRequest* request,
    GetCostValuesAndWeightsResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::GetCostValuesAndWeights(request, &agent_, model,
                                                  data_, response);
}

grpc::Status AgentService::PlannerStep(grpc::ServerContext* context,
                                       const PlannerStepRequest* request,
                                       PlannerStepResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  agent_.plan_enabled = true;
  agent_.PlanIteration(&thread_pool_);

  return grpc::Status::OK;
}

grpc::Status AgentService::Step(grpc::ServerContext* context,
                                const StepRequest* request,
                                StepResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  mjpc::State& state = agent_.ActiveState();
  state.CopyTo(model, data_);
  agent_.ActiveTask()->Transition(model, data_);
  agent_.ActivePlanner().ActionFromPolicy(data_->ctrl, state.state().data(),
                                          state.time(),
                                          request->use_previous_policy());
  mj_step(model, data_);
  state.Set(model, data_);
  return grpc::Status::OK;
}

grpc::Status AgentService::Reset(grpc::ServerContext* context,
                                 const ResetRequest* request,
                                 ResetResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::Reset(&agent_, agent_.GetModel(), data_);
}

grpc::Status AgentService::SetTaskParameters(
    grpc::ServerContext* context, const SetTaskParametersRequest* request,
    SetTaskParametersResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::SetTaskParameters(request, &agent_);
}

grpc::Status AgentService::GetTaskParameters(
    grpc::ServerContext* context, const GetTaskParametersRequest* request,
    GetTaskParametersResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::GetTaskParameters(request, &agent_, response);
}

grpc::Status AgentService::SetCostWeights(
    grpc::ServerContext* context, const SetCostWeightsRequest* request,
    SetCostWeightsResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::SetCostWeights(request, &agent_);
}

grpc::Status AgentService::SetMode(grpc::ServerContext* context,
                                   const SetModeRequest* request,
                                   SetModeResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::SetMode(request, &agent_);
}

grpc::Status AgentService::GetMode(grpc::ServerContext* context,
                                   const GetModeRequest* request,
                                   GetModeResponse* response) {
  if (!Initialized()) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "Init not called."};
  }
  return grpc_agent_util::GetMode(request, &agent_, response);
}

}  // namespace agent_grpc
