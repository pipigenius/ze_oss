// Copyright (c) 2015-2016, ETH Zurich, Wyss Zurich, Zurich Eye
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the ETH Zurich, Wyss Zurich, Zurich Eye nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL ETH Zurich, Wyss Zurich, Zurich Eye BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <ze/vi_simulation/camera_simulator.hpp>

#include <ze/cameras/camera_rig.hpp>
#include <ze/cameras/camera_utils.hpp>
#include <ze/common/random_matrix.hpp>
#include <ze/vi_simulation/trajectory_simulator.hpp>
#include <ze/visualization/viz_interface.hpp>

namespace ze {

// -----------------------------------------------------------------------------
void CameraSimulator::initializeMap()
{
  uint32_t num_frames = options_.max_num_landmarks_
                         / options_.num_keypoints_per_frame;
  real_t time = trajectory_->start();
  real_t dt = (trajectory_->end() - time) / num_frames;
  int num_landmarks_per_frame = options_.num_keypoints_per_frame / rig_->size();
  uint32_t num_landmarks = 0u;
  landmarks_W_.resize(Eigen::NoChange, options_.max_num_landmarks_);

  for (uint32_t i = 0u; i < num_frames; ++i)
  {
    Transformation T_W_B = trajectory_->T_W_B(time);

    for (uint32_t cam_idx = 0u; cam_idx < rig_->size(); ++cam_idx)
    {
      // Check how many landmarks are already visible:
      CameraMeasurements measurements = visibleLandmarks(cam_idx, T_W_B, 0u, num_landmarks);
      int num_visible = measurements.keypoints_.cols();
      if (num_visible >= num_landmarks_per_frame)
      {
        continue;
      }

      // Initialize new random visible landmarks.
      int32_t num_new_landmarks = std::max(0, num_landmarks_per_frame - num_visible);
      CHECK_GE(num_new_landmarks, 0);

      Positions p_C;
      std::tie(std::ignore, std::ignore, p_C) =
          generateRandomVisible3dPoints(
            rig_->at(cam_idx), num_new_landmarks,
            10u, options_.min_depth_m, options_.max_depth_m);

      DEBUG_CHECK_LE(static_cast<int>(num_landmarks + num_new_landmarks),
                     landmarks_W_.cols());

      landmarks_W_.middleCols(num_landmarks, num_new_landmarks)
          =  (T_W_B * rig_->T_B_C(cam_idx)).transformVectorized(p_C);

      num_landmarks += num_new_landmarks;
    }
    time += dt;
  }

  VLOG(1) << "Initialized map with " << num_landmarks << " visible landmarks.";
  landmarks_W_.conservativeResize(Eigen::NoChange, num_landmarks);
}

// -----------------------------------------------------------------------------
CameraMeasurements CameraSimulator::visibleLandmarks(
    const uint32_t cam_idx,
    const Transformation& T_W_B,
    const uint32_t lm_min_idx,
    const uint32_t lm_max_idx)
{
  auto t = timer_[SimTimer::visible_landmarks].timeScope();

  const uint32_t num_landmarks = lm_max_idx - lm_min_idx;
  if (num_landmarks == 0)
  {
    return CameraMeasurements();
  }

  const Size2u image_size = rig_->at(cam_idx).size();
  const auto lm_W = landmarks_W_.middleCols(lm_min_idx, num_landmarks);
  const auto lm_C = (T_W_B * rig_->T_B_C(cam_idx)).inverse().transformVectorized(lm_W);
  Keypoints px = rig_->at(cam_idx).projectVectorized(lm_C);
  std::vector<uint32_t> visible_indices;
  for (uint32_t i = 0u; i < num_landmarks; ++i)
  {
    if (lm_C(2,i) < options_.min_depth_m ||
        lm_C(2,i) > options_.max_depth_m)
    {
      // Landmark is either behind or too far from the camera.
      continue;
    }

    if (isVisible(image_size, px.col(i)))
    {
      visible_indices.push_back(i);
    }
  }

  // Copy visible indices into Camera Measurements struct:
  CameraMeasurements m;
  m.keypoints_.resize(Eigen::NoChange, visible_indices.size());
  m.global_landmark_ids_.resize(visible_indices.size());
  for (size_t i = 0; i < visible_indices.size(); ++i)
  {
    m.keypoints_.col(i) = px.col(visible_indices[i]);
    m.global_landmark_ids_[i] = visible_indices[i];
  }

  return m;
}

// -----------------------------------------------------------------------------
CameraMeasurementsVector CameraSimulator::getMeasurements(real_t time)
{
  CHECK_GT(landmarks_W_.cols(), 0) << "Map has not been initialized.";
  CHECK_GE(time, trajectory_->start());
  CHECK_LT(time, trajectory_->end());

  auto t = timer_[SimTimer::get_measurements].timeScope();

  Transformation T_W_B = trajectory_->T_W_B(time);
  std::unordered_map<int32_t, int32_t> new_global_lm_id_to_track_id_map;
  CameraMeasurementsVector measurements;

  for (uint32_t cam_idx = 0u; cam_idx < rig_->size(); ++cam_idx)
  {
    CameraMeasurements m = visibleLandmarks(cam_idx, T_W_B, 0u, landmarks_W_.cols());
    m.local_track_ids_.resize(m.keypoints_.cols());
    for (int32_t i = 0; i < m.keypoints_.cols(); ++i)
    {
      const int32_t lm_id = m.global_landmark_ids_[i];
      int32_t track_id = -1;
      auto res = global_lm_id_to_track_id_map_.find(lm_id);
      if (res == global_lm_id_to_track_id_map_.end())
      {
        // This is a new track:
        track_id = track_id_counter_;
        ++track_id_counter_;
      }
      else
      {
        // This is an existing track:
        track_id = res->second;
      }
      m.local_track_ids_[i] = track_id;

      // Update our list of tracks:
      new_global_lm_id_to_track_id_map[lm_id] = track_id;
    }
    measurements.push_back(m);
  }

  // Update our list of active tracks:
  global_lm_id_to_track_id_map_ = new_global_lm_id_to_track_id_map;

  return measurements;
}

// -----------------------------------------------------------------------------
CameraMeasurementsVector CameraSimulator::getMeasurementsCorrupted(real_t time)
{
  CameraMeasurementsVector measurements = getMeasurements(time);
  for (CameraMeasurements& m : measurements)
  {
    m.keypoints_ += randomMatrixNormalDistributed(2, m.keypoints_.cols(), false,
                                                  0.0, options_.keypoint_noise_sigma);
  }
  return measurements;
}

// -----------------------------------------------------------------------------
void CameraSimulator::reset()
{
  global_lm_id_to_track_id_map_.clear();
}

// -----------------------------------------------------------------------------
void CameraSimulator::setVisualizer(const std::shared_ptr<Visualizer>& visualizer)
{
  viz_ = visualizer;
}

// -----------------------------------------------------------------------------
void CameraSimulator::visualize(
    real_t dt,
    real_t marker_size_trajectory,
    real_t marker_size_landmarks)
{
  CHECK_GT(landmarks_W_.cols(), 0) << "Map has not been initialized.";
  CHECK(viz_) << "No visualizer has been registered.";
  std::vector<Position> trajectory;
  for (real_t time = trajectory_->start(), time_end = trajectory_->end();
       time < time_end; time += dt)
  {
    trajectory.push_back(trajectory_->T_W_B(time).getPosition());
  }
  viz_->drawTrajectory("simulation_trajectory", 0, trajectory, Colors::Green, marker_size_trajectory);
  viz_->drawPoints("simulated_map", 0u, landmarks_W_, Colors::Orange, marker_size_landmarks);
}

} // namespace ze
