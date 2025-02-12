// Copyright 2015 Open Source Robotics Foundation, Inc.
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

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "opencv2/highgui/highgui.hpp"

#include "rclcpp/rclcpp.hpp"

#include "sensor_msgs/msg/image.hpp"
#include "std_msgs/msg/bool.hpp"

#include "image_tools/options.hpp"

#include "./burger.hpp"

/// Convert an OpenCV matrix encoding type to a string format recognized by sensor_msgs::Image.
/**
 * \param[in] mat_type The OpenCV encoding type.
 * \return A string representing the encoding type.
 */
std::string
mat_type2encoding(int mat_type)
{
  switch (mat_type) {
    case CV_8UC1:
      return "mono8";
    case CV_8UC3:
      return "bgr8";
    case CV_16SC1:
      return "mono16";
    case CV_8UC4:
      return "rgba8";
    default:
      throw std::runtime_error("Unsupported encoding type");
  }
}

/// Convert an OpenCV matrix (cv::Mat) to a ROS Image message.
/**
 * \param[in] frame The OpenCV matrix/image to convert.
 * \param[in] frame_id ID for the ROS message.
 * \param[out] Allocated shared pointer for the ROS Image message.
 */
void convert_frame_to_message(
  const cv::Mat & frame, size_t frame_id, sensor_msgs::msg::Image & msg)
{
  // copy cv information into ros message
  msg.height = frame.rows;
  msg.width = frame.cols;
  msg.encoding = mat_type2encoding(frame.type());
  msg.step = static_cast<sensor_msgs::msg::Image::_step_type>(frame.step);
  size_t size = frame.step * frame.rows;
  msg.data.resize(size);
  memcpy(&msg.data[0], frame.data, size);
  msg.header.frame_id = std::to_string(frame_id);
}

int main(int argc, char * argv[])
{
  // Pass command line arguments to rclcpp.
  rclcpp::init(argc, argv);
  std::string device = "/dev/video0";
  std::string topic = "image";
  size_t width = 640;
  size_t height = 480;
  double freq = 30.0;
  // Initialize default demo parameters
  bool show_camera = false;
  size_t depth = rmw_qos_profile_default.depth;
  rmw_qos_reliability_policy_t reliability_policy = rmw_qos_profile_default.reliability;
  rmw_qos_history_policy_t history_policy = rmw_qos_profile_default.history;

  bool burger_mode = false;

  // Force flush of the stdout buffer.
  // This ensures a correct sync of all prints
  // even when executed simultaneously within a launch file.
  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  // Initialize a ROS 2 node to publish images read from the OpenCV interface to the camera.
  auto node = rclcpp::Node::make_shared("cam2image");
  rclcpp::Logger node_logger = node->get_logger();
  
  node->declare_parameter("device");
  node->declare_parameter("topic");
  node->declare_parameter("width");
  node->declare_parameter("height");
  node->declare_parameter("freq");
  node->get_parameter_or<std::string>("device", device, "/dev/video0");
  node->get_parameter_or<std::string>("topic", topic, "image");
  node->get_parameter_or<size_t>("width", width, 640);
  node->get_parameter_or<size_t>("height", height, 480);
  node->get_parameter_or<double>("freq", freq, 30.0);
  

  // Set the parameters of the quality of service profile. Initialize as the default profile
  // and set the QoS parameters specified on the command line.
  auto qos = rclcpp::QoS(
    rclcpp::QoSInitialization(
      // The history policy determines how messages are saved until taken by the reader.
      // KEEP_ALL saves all messages until they are taken, up to a system resource limit.
      // KEEP_LAST enforces a limit on the number of messages that are saved.
      // The limit is specified by the history "depth" parameter.
      history_policy,
      // Depth represents how many messages to save in history when the history policy is KEEP_LAST.
      depth));

  // The reliability policy can be reliable, meaning that the underlying transport layer will try
  // ensure that every message gets received in order, or best effort, meaning that the transport
  // makes no guarantees about the order or reliability of delivery.
  qos.reliability(reliability_policy);

  RCLCPP_INFO(node_logger, "Publishing data on topic '%s'", topic.c_str());
  // Create the image publisher with our custom QoS profile.
  auto pub = node->create_publisher<sensor_msgs::msg::Image>(topic, qos);
  
  

  // is_flipped will cause the incoming camera image message to flip about the y-axis.
  bool is_flipped = false;

  // Subscribe to a message that will toggle flipping or not flipping, and manage the state in a
  // callback.
  auto callback =
    [&is_flipped, &node_logger](const std_msgs::msg::Bool::SharedPtr msg) -> void
    {
      is_flipped = msg->data;
      RCLCPP_INFO(node_logger, "Set flip mode to: %s", is_flipped ? "on" : "off");
    };

  // Set the QoS profile for the subscription to the flip message.
  auto sub = node->create_subscription<std_msgs::msg::Bool>(
    "flip_image", rclcpp::SensorDataQoS(), callback);

  // Set a loop rate for our main event loop.
  rclcpp::WallRate loop_rate(freq);

  cv::VideoCapture cap;
  burger::Burger burger_cap;
  if (!burger_mode) {
    // Initialize OpenCV video capture stream.
    // Always open device 0.
    cap.open(device,cv::CAP_V4L2);

    // Set the width and height based on command line arguments.
    cap.set(cv::CAP_PROP_FRAME_WIDTH, static_cast<double>(width));
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, static_cast<double>(height));
    if (!cap.isOpened()) {
      RCLCPP_ERROR(node_logger, "Could not open video stream");
      return 1;
    }
  }

  // Initialize OpenCV image matrices.
  cv::Mat frame;
  cv::Mat flipped_frame;

  size_t i = 1;

  // Our main event loop will spin until the user presses CTRL-C to exit.
  while (rclcpp::ok()) {
    // Initialize a shared pointer to an Image message.
    auto msg = std::make_unique<sensor_msgs::msg::Image>();
    msg->is_bigendian = false;
    // Get the frame from the video capture.
    if (burger_mode) {
      frame = burger_cap.render_burger(width, height);
    } else {
      cap >> frame;
    }
    // Check if the frame was grabbed correctly
    if (!frame.empty()) {
      // Convert to a ROS image
      if (!is_flipped) {
        convert_frame_to_message(frame, i, *msg);
      } else {
        // Flip the frame if needed
        cv::flip(frame, flipped_frame, 1);
        convert_frame_to_message(flipped_frame, i, *msg);
      }
      if (show_camera) {
        cv::Mat cvframe = frame;
        // Show the image in a window called "cam2image".
        cv::imshow("cam2image", cvframe);
        // Draw the image to the screen and wait 1 millisecond.
        cv::waitKey(1);
      }
      // Publish the image message and increment the frame_id.
      //RCLCPP_INFO(node_logger, "Publishing image #%zd", i);
      pub->publish(std::move(msg));
      ++i;
    }
    // Do some work in rclcpp and wait for more to come in.
    rclcpp::spin_some(node);
    loop_rate.sleep();
  }

  rclcpp::shutdown();

  return 0;
}
