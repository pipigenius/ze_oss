#pragma once

#include <map>
#include <string>
#include <memory>
#include <vector>

#include <rosbag/bag.h>
#include <rosbag/view.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>
#include <ze_eval_node/bmx055_acc.h>
#include <ze_eval_node/bmx055_gyr.h>

#include <ze/data_provider/data_provider_base.hpp>

namespace ze {

class DataProviderRosbag : public DataProviderBase
{
public:
  // optional: imu_topics, required: camera_topics
  DataProviderRosbag(
      const std::string& bag_filename,
      const std::map<std::string, size_t>& imu_topics,
      const std::map<std::string, size_t>& camera_topics);

  // optional: accel_topics, gyro_topics
  DataProviderRosbag(
      const std::string& bag_filename,
      const std::map<std::string, size_t>& gyro_topics,
      const std::map<std::string, size_t>& accel_topics,
      const std::map<std::string, size_t>& camera_topics);

  virtual ~DataProviderRosbag() = default;

  virtual bool spinOnce() override;

  virtual bool ok() const override;

  virtual size_t imuCount() const;

  virtual size_t cameraCount() const;

  size_t size() const;

private:
  void loadRosbag(const std::string& bag_filename);
  void initBagView(const std::vector<std::string>& topics);

  inline bool cameraSpin(const sensor_msgs::ImageConstPtr m_img,
                         const rosbag::MessageInstance& m);
  inline bool imuSpin(const sensor_msgs::ImuConstPtr m_imu,
                      const rosbag::MessageInstance& m);
  inline bool accelSpin(const ze_eval_node::bmx055_accConstPtr m_acc,
                        const rosbag::MessageInstance& m);
  inline bool gyroSpin(const ze_eval_node::bmx055_gyrConstPtr m_gyr,
                       const rosbag::MessageInstance& m);

  std::unique_ptr<rosbag::Bag> bag_;
  std::unique_ptr<rosbag::View> bag_view_;
  rosbag::View::iterator bag_view_it_;
  int n_processed_images_ = 0;

  // subscribed topics:
  std::map<std::string, size_t> img_topic_camidx_map_; // camera_topic --> camera_id
  std::map<std::string, size_t> imu_topic_imuidx_map_; // imu_topic --> imu_id

  std::map<std::string, size_t> accel_topic_imuidx_map_; // accel_topic --> imu_id
  std::map<std::string, size_t> gyro_topic_imuidx_map_; // gyro_topic --> imu_id

  //! Do we operate on split ros messages or the combined imu messages?
  bool uses_split_messages_;

  int64_t last_imu_stamp_ = -1;
  int64_t last_acc_stamp_ = -1;
  int64_t last_gyr_stamp_ = -1;
};

} // namespace ze
