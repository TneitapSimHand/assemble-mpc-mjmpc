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

#include <mujoco/mujoco.h>

#include <vector>

#include "gtest/gtest.h"
#include "mjpc/estimators/estimator.h"
#include "mjpc/test/load.h"
#include "mjpc/threadpool.h"
#include "mjpc/utilities.h"

namespace mjpc {
namespace {

TEST(FiniteDifferenceVelocityAcceleration, Particle2D) {
  // load model
  mjModel* model = LoadTestModel("estimator/particle/task.xml");
  mjData* data = mj_makeData(model);

  // threadpool
  ThreadPool pool(2);

  // dimensions
  int nq = model->nq, nv = model->nv, nu = model->nu, ns = model->nsensordata;

  // ----- simulate ----- //

  // controller
  auto controller = [](double* ctrl, double time) {
    ctrl[0] = mju_sin(10 * time);
    ctrl[1] = mju_cos(10 * time);
  };

  // trajectories
  int T = 5;
  std::vector<double> qpos(nq * (T + 1));
  std::vector<double> qvel(nv * (T + 1));
  std::vector<double> qacc(nv * T);
  std::vector<double> ctrl(nu * T);
  std::vector<double> qfrc_actuator(nv * T);
  std::vector<double> sensordata(ns * (T + 1));

  // reset
  mj_resetData(model, data);

  // rollout
  for (int t = 0; t < T; t++) {
    // set control
    controller(data->ctrl, data->time);

    // forward computes instantaneous qacc
    mj_forward(model, data);

    // cache
    mju_copy(qpos.data() + t * nq, data->qpos, nq);
    mju_copy(qvel.data() + t * nv, data->qvel, nv);
    mju_copy(qacc.data() + t * nv, data->qacc, nv);
    mju_copy(ctrl.data() + t * nu, data->ctrl, nu);
    mju_copy(qfrc_actuator.data() + t * nv, data->qfrc_actuator, nv);
    mju_copy(sensordata.data() + t * ns, data->sensordata, ns);

    // step using mj_Euler since mj_forward has been called
    // see mj_ step implementation here
    // https://github.com/deepmind/mujoco/blob/main/src/engine/engine_forward.c#L831
    mj_Euler(model, data);
  }

  // final cache
  mju_copy(qpos.data() + T * nq, data->qpos, nq);
  mju_copy(qvel.data() + T * nv, data->qvel, nv);
  mju_copy(sensordata.data() + T * model->nsensor, data->sensordata, ns);

  // ----- estimator ----- //

  // initialize
  Estimator estimator;
  estimator.Initialize(model);
  mju_copy(estimator.configuration_.Data(), qpos.data(), nq * (T + 1));

  // compute velocity, acceleration
  estimator.ConfigurationToVelocityAcceleration();

  // velocity error
  std::vector<double> velocity_error(nv * T);
  mju_sub(velocity_error.data(), estimator.velocity_.Data() + nv,
          qvel.data() + nv, nv * (T - 1));

  // velocity test
  EXPECT_NEAR(mju_norm(velocity_error.data(), nv * (T - 1)) / (nv * (T - 1)),
              0.0, 1.0e-3);

  // acceleration error
  std::vector<double> acceleration_error(nv * T);
  mju_sub(acceleration_error.data(), estimator.acceleration_.Data() + nv,
          qacc.data() + nv, nv * (T - 2));

  // velocity test
  EXPECT_NEAR(
      mju_norm(acceleration_error.data(), nv * (T - 1)) / (nv * (T - 1)), 0.0,
      1.0e-3);

  // delete data + model
  mj_deleteData(data);
  mj_deleteModel(model);
}

TEST(FiniteDifferenceVelocityAcceleration, Box3D) {
  // load model
  mjModel* model = LoadTestModel("estimator/box/task0.xml");
  mjData* data = mj_makeData(model);

  // threadpool
  ThreadPool pool(2);

  // dimensions
  int nq = model->nq, nv = model->nv, nu = model->nu, ns = model->nsensordata;

  // ----- simulate ----- //
  // trajectories
  int T = 5;
  std::vector<double> qpos(nq * (T + 1));
  std::vector<double> qvel(nv * (T + 1));
  std::vector<double> qacc(nv * T);
  std::vector<double> ctrl(nu * T);
  std::vector<double> qfrc_actuator(nv * T);
  std::vector<double> sensordata(ns * (T + 1));

  // reset
  mj_resetData(model, data);

  // initialize TODO(taylor): improve initialization
  double qpos0[7] = {0.1, 0.2, 0.3, 1.0, 0.0, 0.0, 0.0};
  double qvel0[6] = {0.4, 0.05, -0.22, 0.01, -0.03, 0.24};
  mju_copy(data->qpos, qpos0, nq);
  mju_copy(data->qvel, qvel0, nv);

  // rollout
  for (int t = 0; t < T; t++) {
    // control
    mju_zero(data->ctrl, model->nu);

    // forward computes instantaneous qacc
    mj_forward(model, data);

    // cache
    mju_copy(qpos.data() + t * nq, data->qpos, nq);
    mju_copy(qvel.data() + t * nv, data->qvel, nv);
    mju_copy(qacc.data() + t * nv, data->qacc, nv);
    mju_copy(ctrl.data() + t * nu, data->ctrl, nu);
    mju_copy(qfrc_actuator.data() + t * nv, data->qfrc_actuator, nv);
    mju_copy(sensordata.data() + t * ns, data->sensordata, ns);

    // step using mj_Euler since mj_forward has been called
    // see mj_ step implementation here
    // https://github.com/deepmind/mujoco/blob/main/src/engine/engine_forward.c#L831
    mj_Euler(model, data);
  }

  // final cache
  mju_copy(qpos.data() + T * nq, data->qpos, nq);
  mju_copy(qvel.data() + T * nv, data->qvel, nv);

  mj_forward(model, data);
  mju_copy(sensordata.data() + T * ns, data->sensordata, ns);

  // ----- estimator ----- //

  // initialize
  Estimator estimator;
  estimator.Initialize(model);
  mju_copy(estimator.configuration_.Data(), qpos.data(), nq * (T + 1));

  // compute velocity, acceleration
  estimator.ConfigurationToVelocityAcceleration();

  // velocity error
  std::vector<double> velocity_error(nv * T);
  mju_sub(velocity_error.data(), estimator.velocity_.Data() + nv,
          qvel.data() + nv, nv * (T - 1));

  // velocity test
  EXPECT_NEAR(mju_norm(velocity_error.data(), nv * (T - 1)) / (nv * (T - 1)),
              0.0, 1.0e-3);

  // acceleration error
  std::vector<double> acceleration_error(nv * T);
  mju_sub(acceleration_error.data(), estimator.acceleration_.Data() + nv,
          qacc.data() + nv, nv * (T - 2));

  // velocity test
  EXPECT_NEAR(
      mju_norm(acceleration_error.data(), nv * (T - 1)) / (nv * (T - 1)), 0.0,
      1.0e-3);

  // delete data + model
  mj_deleteData(data);
  mj_deleteModel(model);
}

}  // namespace
}  // namespace mjpc
