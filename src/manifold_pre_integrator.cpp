// Copyright (C) ETH Zurich, Wyss Zurich, Zurich Eye - All Rights Reserved
// Unauthorized copying of this file, via any medium is strictly prohibited
// Proprietary and confidential

#include <ze/imu_evaluation/manifold_pre_integrator.hpp>

namespace ze {

//------------------------------------------------------------------------------
ManifoldPreIntegrationState::ManifoldPreIntegrationState(
    Matrix3 gyro_noise_covariance,
    PreIntegrator::IntegratorType integrator_type,
    bool useSimpleCovariancePropagation,
    const preintegrated_orientation_container_t* D_R_i_k_reference)
  : PreIntegrator(gyro_noise_covariance, integrator_type)
  , D_R_i_k_reference_(D_R_i_k_reference)
  , useSimpleCovariancePropagation_(useSimpleCovariancePropagation)
{
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::integrate(
    times_container_t stamps,
    measurements_container_t measurements)
{
  switch (integrator_type_)
  {
    case PreIntegrator::FirstOrderForward:
      integrateFirstOrderFwd(stamps, measurements);
      break;
    case PreIntegrator::FirstOrderMidward:
      integrateFirstOrderMid(stamps, measurements);
      break;
    default:
      throw std::runtime_error("No valid integrator supplied");
  }

  times_raw_.push_back(stamps.back());

  // Explicitly push beginning if times_ is empty.
  if (times_.size() == 0)
  {
    times_.push_back(times_raw_.front());
  }

  times_.push_back(times_raw_.back());

  // Push the keyframe sampled pre-integration states:
  D_R_i_j_.push_back(D_R_i_k_.back());

  if (compute_absolutes_)
  {
    R_i_j_.push_back(R_i_j_.back() * D_R_i_j_.back());
  }

  // push covariance
  covariance_i_j_.push_back(covariance_i_k_.back());
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::integrateFirstOrderFwd(
    times_container_t stamps,
    measurements_container_t measurements)
{
  // Integrate measurements between frames.
  for (int i = 0; i < measurements.cols() - 1; ++i)
  {
    real_t dt = stamps[i+1] - stamps[i];

    Vector6 measurement = measurements.col(i);
    Vector3 gyro_measurement = measurement.tail<3>(3);


    // Reset to 0 at every step:
    if (i == 0)
    {
      setInitialValuesFwd(dt, gyro_measurement);
    }
    else
    {
      integrateStepFwd(dt, gyro_measurement);
    }

    times_raw_.push_back(stamps[i]);
  }
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::integrateFirstOrderMid(
    times_container_t stamps,
    measurements_container_t measurements)
{
  // Integrate measurements between frames.
  for (int i = 0; i < measurements.cols() - 1; ++i)
  {
    real_t dt = stamps[i+1] - stamps[i];

    Vector6 measurement = measurements.col(i);
    Vector3 gyro_measurement = measurement.tail<3>(3);
    Vector3 gyro_measurement2 = measurements.col(i+1).tail<3>(3);

    // Reset to 0 at every step:
    if (i == 0)
    {
      setInitialValuesMid(dt, gyro_measurement, gyro_measurement2);
    }
    else
    {
      integrateStepMid(dt, gyro_measurement, gyro_measurement2);
    }

    times_raw_.push_back(stamps[i]);
  }
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::setInitialValuesFwd(
    const real_t dt,
    const Eigen::Ref<Vector3>& gyro_measurement)
{
  Matrix3 increment = Quaternion::exp(gyro_measurement * dt).getRotationMatrix();

  // D_R_i_k restarts with every push to the container.
  D_R_i_k_.push_back(Matrix3::Identity());
  if (compute_absolutes_)
  {
    R_i_k_.push_back(R_i_k_.back() * increment);
  }
  covariance_i_k_.push_back(Matrix3::Zero());
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::integrateStepFwd(
    const real_t dt,
    const Eigen::Ref<Vector3>& gyro_measurement)
{
  timers_[IntegrationTimer::integrate].start();

  Matrix3 increment = Quaternion::exp(gyro_measurement * dt).getRotationMatrix();

  D_R_i_k_.push_back(D_R_i_k_.back() * increment);
  if (compute_absolutes_)
  {
    R_i_k_.push_back(R_i_k_.back() * increment);
  }

  // Propagate Covariance:
  Matrix3 J_r;
  if (useSimpleCovariancePropagation_)
  {
     J_r = Matrix3::Identity();
  }
  else
  {
    J_r = expmapDerivativeSO3(gyro_measurement * dt);
  }
  // Covariance of the discrete process.
  Matrix3 gyro_noise_covariance_d = gyro_noise_covariance_ / dt;

  Matrix3 D_R_i_k;
  if (D_R_i_k_reference_)
  {
    D_R_i_k = (*D_R_i_k_reference_)[D_R_i_k_.size() - 1];
  }
  else
  {
    D_R_i_k = D_R_i_k_.back();
  }

  covariance_i_k_.push_back(
        increment.transpose() * covariance_i_k_.back() * increment

        + J_r * gyro_noise_covariance_d * dt * dt * J_r.transpose());

  timers_[IntegrationTimer::integrate].stop();
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::setInitialValuesMid(
    const real_t dt,
    const Eigen::Ref<Vector3>& gyro_measurement,
    const Eigen::Ref<Vector3>& gyro_measurement2)
{
  Matrix3 increment = Quaternion::exp((gyro_measurement + gyro_measurement2) * 0.5 * dt).getRotationMatrix();

  // D_R_i_k restarts with every push to the container.
  D_R_i_k_.push_back(Matrix3::Identity());
  if (compute_absolutes_)
  {
    R_i_k_.push_back(R_i_k_.back() * increment);
  }
  covariance_i_k_.push_back(Matrix3::Zero());
}

//------------------------------------------------------------------------------
void ManifoldPreIntegrationState::integrateStepMid(
    const real_t dt,
    const Eigen::Ref<Vector3>& gyro_measurement,
    const Eigen::Ref<Vector3>& gyro_measurement2)
{
  timers_[IntegrationTimer::integrate].start();

  Matrix3 increment = Quaternion::exp(
                        (gyro_measurement + gyro_measurement2) * 0.5 * dt)
                      .getRotationMatrix();

  D_R_i_k_.push_back(D_R_i_k_.back() * increment);
  if (compute_absolutes_)
  {
    R_i_k_.push_back(R_i_k_.back() * increment);
  }

  // Propagate Covariance:
  Matrix3 J_r;
  if (useSimpleCovariancePropagation_)
  {
     J_r = Matrix3::Identity();
  }
  else
  {
    J_r = expmapDerivativeSO3((gyro_measurement + gyro_measurement2) * 0.5 * dt);
  }

  // Covariance of the discrete process.
  Matrix3 gyro_noise_covariance_d = gyro_noise_covariance_ / dt;

  Matrix3 D_R_i_k;
  if (D_R_i_k_reference_)
  {
    D_R_i_k = (*D_R_i_k_reference_)[D_R_i_k_.size() - 1];
  }
  else
  {
    D_R_i_k = D_R_i_k_.back();
  }

  covariance_i_k_.push_back(
        increment.transpose() * covariance_i_k_.back() * increment
        + J_r * gyro_noise_covariance_d * dt * dt * J_r.transpose());

  timers_[IntegrationTimer::integrate].stop();
}

} // namespace ze
