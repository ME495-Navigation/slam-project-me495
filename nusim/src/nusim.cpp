//! @file nu turtlebot simulator
//! @brief Simulator node for turtlebot.
// Parameters:
//    rate: int - frequency of simulation timer updates (hz)
// Parameters for robot itself
//    motor_cmd_max: int - max motor cmd value
//    motor_cmd_per_rad_sec: double - ratio between motor cmd and rad/sec
//    encoder_ticks_per_rad: double - ratio from encoder tick to rad/sec
//    track_width: double - track-width of the robot simulating
//    wheel_radius: double - wheel radius of the robot simulating
//    collision_radius: double - collision radius of robot (used to simulate
//    simple collision)
// parameter of the simulated world
//    x0: double - Initial x position
//    y0: double - Initial y position
//    theta0: double - Initial theta position
//    arena_x_length: double - x length of arena
//    arena_y_length: double - y length of arena
//    obstacles/x: vector<double> - List of obstical's x coordinates
//    obstacles/y: vector<double> - List of obstical's y coordinates
//    obstacles/r: double - obstacle's radius, all obstacle share this radius
// Noise in robot's motion
//    input_noise: double - motor cmd noise, applied to how robot move (and
//    tracked) slip_fraction: double - wheel slippage, affect the encoder
//    reading to be slightly off
// Fake sensor param
//    max_range: double - max range of fake sensor noise.
//    basic_sensor_variance: double - the fake sensor variance.

//
// Publishers:
//   /nusim/obstacles: visualization_msgs/msg/MarkerArray
//   /nusim/timestep: std_msgs/msg/UInt64
//   /nusim/walls: visualization_msgs/msg/MarkerArray
//   /parameter_events: rcl_interfaces/msg/ParameterEvent
//   /fake_sensor: visualization_msgs::msg::MarkerArray
//   ~/wall: visualization_msgs::msg::MarkerArray
//   ~/obstacles: visualization_msgs::msg::MarkerArray
//   /tf: tf2_msgs/msg/TFMessage
//
// Service Servers:
//   /nusim/reset: std_srvs/srv/Empty
//   /nusim/teleport: nusim/srv/Teleport

#include <cstddef>
#include <rclcpp/parameter_value.hpp>
#include <rclcpp/subscription.hpp>
#include <rmw/qos_profiles.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_ros/transform_broadcaster.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <memory>
#include <nuturtlebot_msgs/msg/sensor_data.hpp>
#include <nuturtlebot_msgs/msg/wheel_commands.hpp>
#include <optional>
#include <random>
#include <rclcpp/qos.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int64.hpp>
#include <std_srvs/srv/empty.hpp>
#include <string>
#include <turtlelib/diff_drive.hpp>
#include <turtlelib/geometry2d.hpp>
#include <turtlelib/se2d.hpp>
#include <vector>
#include <visualization_msgs/msg/marker.hpp>
#include <visualization_msgs/msg/marker_array.hpp>

#include "nusim/srv/teleport.hpp"
#include <nav_msgs/msg/path.hpp>

#include "leo_ros_utils/math_helper.hpp"
#include "leo_ros_utils/param_helper.hpp"
#include <geometry_msgs/msg/pose_with_covariance.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>

namespace {

std::random_device rd{};
std::mt19937 rand_eng{rd()};
auto get_transient_local_qos() {
  auto profile = rmw_qos_profile_default;
  profile.durability = rmw_qos_durability_policy_e::RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  // rclcpp::QoSInitialization(rclcpp::KeepAll(),profile)
  rclcpp::QoS transient_local_qos(rclcpp::KeepLast(2), profile);
  transient_local_qos.durability(rclcpp::DurabilityPolicy::TransientLocal);

  return transient_local_qos;
}
auto transient_local_qos = get_transient_local_qos();

// markers generated from here missing: header, id, action,
std::vector<visualization_msgs::msg::Marker> GenObstacles(std::vector<double> x_s,
                                                          std::vector<double> y_s, double radius) {
  std::vector<visualization_msgs::msg::Marker> out;

  for (size_t i = 0; i < x_s.size(); ++i) {

    visualization_msgs::msg::Marker obstacle_marker;
    obstacle_marker.type = obstacle_marker.CYLINDER;
    obstacle_marker.scale.x = radius * 2;
    obstacle_marker.scale.y = radius * 2;
    obstacle_marker.scale.z = 0.25;
    obstacle_marker.pose.position.x = x_s.at(i);
    obstacle_marker.pose.position.y = y_s.at(i);
    obstacle_marker.pose.position.z = 0.25 / 2;
    obstacle_marker.color.r = 1.0;
    obstacle_marker.color.a = 0.8;

    // If the center offset is a thing, try to calculate a new xy.
    out.push_back(obstacle_marker); // P_world_obs
  }
  return out;
}

struct LaserParam {
  double range_max;
  double range_min;
  double angle_increment;
  int number_of_sample;
  double resolution;
  double noise_level;
};

std::vector<std::pair<turtlelib::Point2D, turtlelib::Point2D>> ArenaCorners(double x_len,
                                                                            double y_len) {
  //
  return {
      {{x_len / 2, y_len / 2}, {-x_len / 2, y_len / 2}},   // right
      {{-x_len / 2, y_len / 2}, {-x_len / 2, -y_len / 2}}, // btm
      {{-x_len / 2, -y_len / 2}, {x_len / 2, -y_len / 2}}, // left
      {{x_len / 2, -y_len / 2}, {x_len / 2, y_len / 2}}    // top
  };
}

const std::string kWorldFrame = "nusim/world";
const std::string kSimRobotBaseFrameID = "red/base_footprint";

} // namespace

using leo_ros_utils::GetParam;

//! @brief This class is the nusim node itself. Holds everything the node needs.
class NuSim : public rclcpp::Node {
public:
  NuSim()
      : Node("nusim"),

        // Ros related params
        update_period(std::chrono::milliseconds(
            1000 / GetParam<int>(*this, "rate", "The rate of simulator", 200))),
        x0(GetParam<double>(*this, "x0", "inital robot x location")),
        y0(GetParam<double>(*this, "y0", "inital robot y location")),
        theta0(GetParam<double>(*this, "theta0", "initial robot theta")),
        motor_cmd_max(GetParam<int>(*this, "motor_cmd_max", "radius of wheel")),
        motor_cmd_per_rad_sec(GetParam<double>(*this, "motor_cmd_per_rad_sec",
                                               "motor cmd per rad/s (actually the inverse)")),
        encoder_ticks_per_rad(
            GetParam<double>(*this, "encoder_ticks_per_rad", "encoder_ticks_per_rad")),
            collision_radius(
            GetParam<double>(*this, "collision_radius", "collision radius of the robot")),

        red_bot(turtlelib::DiffDrive{
            GetParam<double>(*this, "track_width", "robot center to wheel-track distance"),
            GetParam<double>(*this, "wheel_radius", "wheel radius"),
            turtlelib::Transform2D{{x0, y0}, theta0}}),
        obstacles_r(GetParam<double>(*this, "obstacles/r", "obstacle radius")),


        // This is quite a crazy hack. Using trinary statement and lambda to do it inside initializer
        // So we can do const on this object.
        static_obstacles(
            (GetParam<std::vector<double>>(*this, "obstacles/x", "list of obstacle's x coord")
                 .size() ==
             GetParam<std::vector<double>>(*this, "obstacles/y", "list of obstacle's y coord")
                 .size())
                ? GenObstacles(get_parameter("obstacles/x").get_value<std::vector<double>>(),
                               get_parameter("obstacles/y").get_value<std::vector<double>>(),
                               get_parameter("obstacles/r").get_value<double>())
                : [&](){
                  RCLCPP_ERROR(get_logger(), "Mismatch obstacle x y numbers");
                  throw;
                  return std::vector<visualization_msgs::msg::Marker>{};
                  }()),
        arena_corners(ArenaCorners(GetParam<double>(*this, "arena_x_length", "x length of arena", 5.0),GetParam<double>(*this, "arena_y_length", "x length of arena", 3.0))),

        // Simulation only params
        input_noise(GetParam<double>(*this, "input_noise",
                                     "input noise variance when applying wheel velocity", 0)),
        slip_fraction(GetParam<double>(*this, "slip_fraction",
                                       "abs range of wheel slip amount on each cycle.", 0)),
        max_range(GetParam<double>(*this, "max_range",
                                   "maxinum range for basic sensor to see a "
                                   "obsticle. negative will disable max range",
                                   -1.0)),

        sim_laser_param({
        GetParam<double>(*this, "laser_range_max",
                                       "max range of sim laser scan", 3.5),
        GetParam<double>(*this, "laser_range_min",
                                       "min range of sim laser scan", 0.12),
        GetParam<double>(*this, "laser_angle_increment",
                                       "angular distance between each laser measurement.", (turtlelib::PI *2 ) / 360),
        GetParam<int>(*this, "laser_number_of_sample",
                                       "number of laser samples.", 360),
        GetParam<double>(*this, "laser_resolution",
                                       "resolution of the laser", 0.0174533),
        GetParam<double>(*this, "laser_noise_level",
                                       "nose level of laser measurement.", 0),
        }),
        // Member variable, not param
        input_gauss_distribution(0.0, input_noise),
        wheel_uniform_distribution(-slip_fraction, slip_fraction),
        basic_sensor_gauss_distribution(
            0.0, GetParam<double>(*this, "basic_sensor_variance",
                                  "variance of noise in basic sensor's reading.", 0))

  {
    // Uncomment this to turn on debug level and enable debug statements
    // rcutils_logging_set_logger_level(get_logger().get_name(),
    // RCUTILS_LOG_SEVERITY_DEBUG);

    // Arena wall stuff
    const double arena_x_length = get_parameter("arena_x_length").get_value<double>();
    const double arena_y_length = get_parameter("arena_y_length").get_value<double>();
    // It's a lot more code to change if I change it to use corner pair lists
    PublishArenaWalls(arena_x_length, arena_y_length);

    PublishStaticObstacles(static_obstacles);

    // Setup pub/sub
    time_step_publisher_ = create_publisher<std_msgs::msg::UInt64>("~/timestep", 10);
    red_sensor_publisher_ =
        create_publisher<nuturtlebot_msgs::msg::SensorData>("red/sensor_data", 10);
    fake_sensor_publisher_ =
        create_publisher<visualization_msgs::msg::MarkerArray>("/fake_sensor", 10);
    path_publisher_ = create_publisher<nav_msgs::msg::Path>("red/path", 10);
    sim_laser_publisher_ = create_publisher<sensor_msgs::msg::LaserScan>("~/laser_scan", 10);
    tf_broadcaster_ = std::make_unique<tf2_ros::TransformBroadcaster>(*this);

    wheel_cmd_listener_ = create_subscription<nuturtlebot_msgs::msg::WheelCommands>(
        "red/wheel_cmd", 10, std::bind(&NuSim::WheelCmdCb, this, std::placeholders::_1));

    // Setup srv/client
    reset_service_ = create_service<std_srvs::srv::Empty>(
        "~/reset",
        std::bind(&NuSim::reset_srv, this, std::placeholders::_1, std::placeholders::_2));
    teleport_service_ = create_service<nusim::srv::Teleport>(
        "~/teleport",
        std::bind(&NuSim::teleport_callback, this, std::placeholders::_1, std::placeholders::_2));

    // Setup timer and set things in motion.
    main_timer_ = this->create_wall_timer(update_period, std::bind(&NuSim::MainTimerStep, this));
    // 5HZ update rate
    fake_sensor_timer_ = this->create_wall_timer(std::chrono::milliseconds{200},
                                                 std::bind(&NuSim::FakeSensorTimerStep, this));
    if (sim_laser_param.number_of_sample > 0) {
      sim_laser_timer_ = this->create_wall_timer(std::chrono::milliseconds{200},
                                                 std::bind(&NuSim::SimLaserTimerStep, this));
    } else {
      RCLCPP_WARN_STREAM(get_logger(),
                         "Requested 0 samples in sim laser ! will not publish this at all");
    }
  }

private:
  // Private functions

  //! @brief Main timer callback function.
  void MainTimerStep() {
    std_msgs::msg::UInt64 time_step_msg;
    time_step_msg.data = ++time_step_;
    time_step_publisher_->publish(time_step_msg);
    auto current_stamp = get_clock()->now();
    std::stringstream debug_ss;
    debug_ss << "\n===>\n";

    // Let's move the red bot
    // Directly using duration constructor does work! (default std::ratio is
    // 1:1) Proof here : https://godbolt.org/z/TxMMqvrKh
    double period_sec = std::chrono::duration<double>(update_period).count();

    // Why I get warning saying int to double is narrowing conversion?
    turtlelib::WheelVelocity raw_cmd_vel = {static_cast<double>(latest_wheel_cmd.left_velocity),
                                            static_cast<double>(latest_wheel_cmd.right_velocity)};
    turtlelib::WheelVelocity wheel_cmd_vel = raw_cmd_vel * motor_cmd_per_rad_sec * period_sec;

    // This is a special catch showing we have jumped more then pi/2 on
    // wheel in one iteration, which should not happen! (but let's just let
    // error play out)
    if (std::abs(wheel_cmd_vel.left) > turtlelib::PI ||
        std::abs(wheel_cmd_vel.right) > turtlelib::PI) {
      RCLCPP_WARN_STREAM(get_logger(),
                         "This steps's wheel increment is more then PI! " << wheel_cmd_vel);
    }

    debug_ss << "wheel_cmd " << raw_cmd_vel << "\n";
    debug_ss << "wheel_vel " << wheel_cmd_vel << "\n";
    // inject noise between wheel command, and how much motor actually turned.
    if (!turtlelib::almost_equal(wheel_cmd_vel.left, 0)) {
      wheel_cmd_vel.left += input_gauss_distribution(rand_eng);
    }
    if (!turtlelib::almost_equal(wheel_cmd_vel.right, 0)) {
      wheel_cmd_vel.left += input_gauss_distribution(rand_eng);
    }
    debug_ss << "wheel_vel with noise" << wheel_cmd_vel << "\n";
    red_bot.UpdateBodyConfigWithVel(wheel_cmd_vel);
    debug_ss << "new bot body " << red_bot.GetBodyConfig() << "\n";

    // Do a collision update.
    if (CollisionUpdate()) {
      debug_ss << "bot body after collision check" << red_bot.GetBodyConfig() << "\n";
    }

    auto new_wheel_config = red_bot.GetWheelConfig();
    debug_ss << "new wheel_config " << new_wheel_config << "\n";
    // The internal tracking of our simulated robot doesn't have wheel slip.
    // Wheel slip only show up as a encoder reading goes off, not robot itself
    // goes off.
    new_wheel_config.left += wheel_uniform_distribution(rand_eng);
    new_wheel_config.right += wheel_uniform_distribution(rand_eng);
    debug_ss << "new wheel_config with noise" << new_wheel_config << "\n";
    // Since we let diff bot to track our wheel config, it needs to know about
    // this slip-age update as well. Or next cycle it will ignore this.
    red_bot.SetWheelConfig(new_wheel_config);

    nuturtlebot_msgs::msg::SensorData red_sensor_msg;
    red_sensor_msg.left_encoder = encoder_ticks_per_rad * new_wheel_config.left;
    red_sensor_msg.right_encoder = encoder_ticks_per_rad * new_wheel_config.right;
    red_sensor_msg.stamp = current_stamp;
    red_sensor_publisher_->publish(red_sensor_msg);
    debug_ss << "encoder sensor value" << nuturtlebot_msgs::msg::to_yaml(red_sensor_msg, true);

    // Publish TF for red robot
    auto tf =
        Gen2DTransform(red_bot.GetBodyConfig(), kWorldFrame, kSimRobotBaseFrameID, current_stamp);
    tf_broadcaster_->sendTransform(tf);
    latest_bot_tf = tf;
    // Publish the robot track path.
    geometry_msgs::msg::PoseStamped new_pose;
    new_pose.pose = leo_ros_utils::Convert(red_bot.GetBodyConfig());
    new_pose.header.frame_id = kWorldFrame;
    new_pose.header.stamp = current_stamp;

    bot_path_history.push_back(new_pose);
    if (bot_path_history.size() >= kRobotPathHistorySize) {
      bot_path_history.pop_front();
    }
    nav_msgs::msg::Path path_msg;
    path_msg.header = new_pose.header;
    path_msg.poses = std::vector<geometry_msgs::msg::PoseStamped>{bot_path_history.begin(),
                                                                  bot_path_history.end()};
    path_publisher_->publish(path_msg);

    RCLCPP_DEBUG_STREAM(get_logger(), debug_ss.str());
  }

  void FakeSensorTimerStep() {
    visualization_msgs::msg::MarkerArray msg;
    size_t i = 0;
    for (const auto &obs : static_obstacles) {
      auto obstacle_marker = obs;
      obstacle_marker.header.frame_id = kSimRobotBaseFrameID;
      obstacle_marker.header.stamp = latest_bot_tf.header.stamp;
      obstacle_marker.id = kFakeSenorStartingID + (i++);

      auto new_loc = red_bot.GetBodyConfig().inv()(
          {turtlelib::Point2D{obs.pose.position.x, obs.pose.position.y}}); // P_center_obs

      double range = turtlelib::Vector2D{new_loc.x, new_loc.y}.magnitude();
      if (range > max_range && max_range >= 0.0) {
        // Because of this, it's easier to merge logic for both usecase, instead
        // of doing these math outside and have this function simple.
        obstacle_marker.action = obstacle_marker.DELETE;
      } else {
        obstacle_marker.action = obstacle_marker.MODIFY;
      }

      // Sensor noise after detection
      obstacle_marker.pose.position.x = new_loc.x + basic_sensor_gauss_distribution(rand_eng);
      obstacle_marker.pose.position.y = new_loc.y + basic_sensor_gauss_distribution(rand_eng);
      obstacle_marker.scale.z = 0.4;
      obstacle_marker.pose.position.z = 0.4 / 2;
      obstacle_marker.color.g = 1.0;
      obstacle_marker.color.a = 0.4;

      msg.markers.push_back(obstacle_marker);

    }

    fake_sensor_publisher_->publish(msg);
  }

  void SimLaserTimerStep() {
    sensor_msgs::msg::LaserScan laser_msg;

    // TODO check if we need to emit laser scan from tip of robot
    laser_msg.header.frame_id = kSimRobotBaseFrameID;
    laser_msg.header.stamp = latest_bot_tf.header.stamp;

    laser_msg.angle_min = 0;
    laser_msg.angle_increment = sim_laser_param.angle_increment;

    laser_msg.time_increment = 0;
    laser_msg.scan_time = 0;

    laser_msg.range_min = sim_laser_param.range_min;
    laser_msg.range_max = sim_laser_param.range_max;
    for (int count = 0; count < sim_laser_param.number_of_sample; count++) {
      auto maybe_dist = ray_hitting(count * sim_laser_param.angle_increment);
      // TODO revert this after debug
      laser_msg.ranges.push_back(maybe_dist.value_or(sim_laser_param.range_max - 1));
    }
    // This assume angle_max is inclusive
    // assume number of sample is >1 (which is checked in constructor)
    laser_msg.angle_max = (sim_laser_param.number_of_sample - 1) * sim_laser_param.angle_increment;
    sim_laser_publisher_->publish(laser_msg);
  }

  //! @param ray_angle_body - angle of the ray in body frame.

  std::optional<double> ray_obstacle_check(turtlelib::Vector2D v_ray_unit,
                                           turtlelib::Vector2D v_bot_obs) {

    if (v_bot_obs.magnitude() - obstacles_r > sim_laser_param.range_max) {
      // Don't care if obstacle is 100% too far away.
      return std::nullopt;
    }

    // ############# Begin Citation [6]#############
    // Project bot_obs onto ray, length of U1 in citation graph
    double proj_obs_ray_mag = turtlelib::dot(v_bot_obs, v_ray_unit);
    // Unless ray origin is inside the obstacle. They won't intersect
    // (regardless how big the circle is) if circle center is not in "front"
    // +-90 of ray. IF the projected magnitude negative, the obs is behind the
    // ray.
    if (proj_obs_ray_mag < 0.0) {
      // This is 50% of the case. for each obstacle, half the ray will skip out
      // here.
      return std::nullopt;
    }
    double circle_to_ray_d = (v_bot_obs - proj_obs_ray_mag * v_ray_unit).magnitude();
    if (circle_to_ray_d > obstacles_r) {
      // This is the case of not intersect
      return std::nullopt;
    }
    // Then we can form the small triangle
    double intersect_offset =
        std::sqrt(obstacles_r * obstacles_r - circle_to_ray_d * circle_to_ray_d);
    // ############# End Citation [6]#############

    // This (proj_obs_ray_mag - intersect_offset) *v_ray_unit is the vector to
    // the closest intersect. It should be positive!
    double ray_length = (proj_obs_ray_mag - intersect_offset);
    if (ray_length < 0.0) {
      RCLCPP_ERROR_STREAM(get_logger(), "Intersection behind the ray! dist: "
                                            << ray_length << "\n Ray unit vector: " << v_ray_unit
                                            << " bot to obs vector: " << v_bot_obs
                                            << "\n projected length on ray " << proj_obs_ray_mag
                                            << " obs to ray dis: " << circle_to_ray_d);
    }

    return ray_length;
  }

  std::optional<double>
  ray_wall_check(turtlelib::Vector2D v_ray_unit,
                 std::pair<turtlelib::Point2D, turtlelib::Point2D> wall_endpoints,
                 turtlelib::Point2D bot_loc) {

    // ############# Citation [7]#############
    // Also see doc/Line_Ray_intersect_math.md for the full math.

    // clang-format off
    //                            ray_direction              P2                     
    //                           ^                      ---->                       
    //                           |              -------/    >                       
    //                           |      -------/          -/ ^                      
    //                          -x-----/                 /   |                      
    //                  -------/ | intersect            /    |                      
    //    P1    -------/         |                    -/     |                      
    //     <---/                 |                   /       |                      
    //       <                   |                  /        |                      
    //     ^  \-                 |                -/ V_a2    |                      
    //     |    \                |               /           |                      
    //     |     \-              |              /            |                      
    //     |       \ V_a1        |            -/             |                      
    //     |        \-           |           /               |                      
    //     |          \          |         -/                |V_b2                  
    // V_b1|           \         |        /                  |                      
    //     |            \-       |       /                   |                      
    //     |              \      |     -/                    |                      
    //     |               \-    |    /                      |                      
    //     |                 \   |   /                       |                      
    //     |   V_c1           \- | -/         V_c2           |        Normal to ray 
    //     <------------------- \|---------------------------->---------->          
    //                            P                                                 
    //
    // clang-format on
    // Bot loc is our p
    auto v_ray_perp = v_ray_unit.Perpendicular();

    turtlelib::Vector2D v_a1 = wall_endpoints.first - bot_loc;
    turtlelib::Vector2D v_a2 = wall_endpoints.second - bot_loc;

    auto b1_proj = turtlelib::dot(v_a1, v_ray_unit);
    auto b2_proj = turtlelib::dot(v_a2, v_ray_unit);
    if (b1_proj < 0 && b2_proj < 0) {
      // This is the case the entire line is behind the ray
      return std::nullopt;
    }

    auto c1_proj = turtlelib::dot(v_a1, v_ray_perp);
    auto c2_proj = turtlelib::dot(v_a2, v_ray_perp);
    // don't know any better way for same sign check

    if (c1_proj * c2_proj > 0) {
      // This is the case the entire line is off to one side of the ray
      return std::nullopt;
    }

    double c1_mag = std::abs(c1_proj);
    double c2_mag = std::abs(c2_proj);

    auto x = (c1_mag * (b2_proj - b1_proj)) / (c1_mag + c2_mag);
    auto intersect_mag = x + b1_proj;
    RCLCPP_DEBUG_STREAM(get_logger(), "v ray " << v_ray_unit << " perp " << v_ray_perp);
    RCLCPP_DEBUG_STREAM(get_logger(), "v_a1 " << v_a1 << " v_a2" << v_a2);
    RCLCPP_DEBUG_STREAM(get_logger(), " b1_proj " << b1_proj << " b2_proj " << b2_proj);
    RCLCPP_DEBUG_STREAM(get_logger(), " c1_proj " << c1_proj << " c2_proj " << c2_proj);
    RCLCPP_DEBUG_STREAM(get_logger(), " c1_mag " << c1_mag << " c2_mag " << c2_mag);
    RCLCPP_DEBUG_STREAM(get_logger(), "X: " << x);
    RCLCPP_DEBUG_STREAM(get_logger(),
                        "Hit at " << intersect_mag << " Vhit " << (intersect_mag)*v_ray_unit);
    if (intersect_mag < 0) {
      return std::nullopt;
    }
    return intersect_mag;
  }

  //! @param ray_angle_body - angle of the ray in body frame.
  std::optional<double> ray_hitting(double ray_angle_body) {

    // We put all calculation in world frame. Its easier for walls.
    // We only need a magnitude measure along a vector ray, so frame is not the problem.
    auto bot_config = red_bot.GetBodyConfig();
    double ray_angle_world = ray_angle_body + bot_config.rotation();

    std::optional<double> closest_intersect = std::nullopt;

    // Unit vector ray in world
    auto v_ray_unit =
        turtlelib::Vector2D{std::cos(ray_angle_world), std::sin(ray_angle_world)}.normalize();


    for (const auto &obs : static_obstacles) {
      // robot to obs center
      auto maybe_point = ray_obstacle_check(
          v_ray_unit,
          turtlelib::Vector2D{obs.pose.position.x, obs.pose.position.y} - bot_config.translation());
      // using short circuit in if here
      if (maybe_point.has_value() &&
          maybe_point.value() < closest_intersect.value_or(sim_laser_param.range_max + 1)) {
        // It's safe to assume this. If we have intersect further then this, we want to discard that
        // anyway
        // The fun part of this is if the ray hits nothing, it will still be nullopt
        closest_intersect = maybe_point.value();
      }
    }

    if (! closest_intersect.has_value()){
      for ( const auto & wall_corner_pair : arena_corners){
        RCLCPP_DEBUG_STREAM(get_logger() , "Wall "<< wall_corner_pair.first << wall_corner_pair.second );
        auto maybe_length = ray_wall_check(v_ray_unit  ,wall_corner_pair, bot_config.translation().ToPoint() );

        if (maybe_length.has_value()){
          closest_intersect = maybe_length.value();
          break;
        }
      }
    }
    return closest_intersect;
  }

  //! @brief service callback for reset
  //! @param / service request (not used)
  //! @param / service respond (not used)
  void reset_srv(const std_srvs::srv::Empty::Request::SharedPtr,
                 std_srvs::srv::Empty::Response::SharedPtr) {
    time_step_ = 0;
    red_bot.SetBodyConfig({{x0, y0}, theta0});
    return;
  }

  //! @brief service callback for teleport
  //! @param req teleport request data
  //! @param / service response, not used
  void teleport_callback(const nusim::srv::Teleport::Request::SharedPtr req,
                         nusim::srv::Teleport::Response::SharedPtr) {
    red_bot.SetBodyConfig({{req->x, req->y}, req->theta});
    return;
  }

  void WheelCmdCb(const nuturtlebot_msgs::msg::WheelCommands &msg) { latest_wheel_cmd = msg; }

  // rclcpp time
  // From https://en.cppreference.com/w/cpp/language/default_arguments
  // A default argument is evaluated each time the function is called with no
  // argument for the corresponding parameter.

  //! @brief Generate a TransformStamped object for 2D transform
  //! @param x x position of transform
  //! @param y y position of transform
  //! @param theta rotation in z axis
  //! @param parent_frame_id frame name for parent
  //! @param child_frame_id frame name for child
  //! @param time_stamp_opt optional time stamp (default to current time)
  //! @return a populated TransformStamped object from given infos.
  geometry_msgs::msg::TransformStamped
  Gen2DTransform(turtlelib::Transform2D trans2d, std::string parent_frame_id,
                 std::string child_frame_id,
                 std::optional<rclcpp::Time> time_stamp_opt = std::nullopt) {

    geometry_msgs::msg::TransformStamped tf_stamped;
    tf_stamped.header.stamp = time_stamp_opt.value_or(get_clock()->now());
    tf_stamped.header.frame_id = parent_frame_id;
    tf_stamped.child_frame_id = child_frame_id;
    tf_stamped.transform.translation.x = trans2d.translation().x;
    tf_stamped.transform.translation.y = trans2d.translation().y;
    tf_stamped.transform.translation.z = 0;

    tf2::Quaternion q;
    q.setRPY(0, 0, trans2d.rotation());
    tf_stamped.transform.rotation.x = q.x();
    tf_stamped.transform.rotation.y = q.y();
    tf_stamped.transform.rotation.z = q.z();
    tf_stamped.transform.rotation.w = q.w();
    return tf_stamped;
  }

  //! @brief Update robot's current config by checking for collision.
  //! If collision happens, Simply push robot off to the side a bit (to a tangent point)
  bool CollisionUpdate() {
    bool collision = false;
    for (const auto &obs : static_obstacles) {
      // Get current robot to obstacle vector
      turtlelib::Vector2D v_obs{obs.pose.position.x, obs.pose.position.y};
      auto v_robot = red_bot.GetBodyConfig().translation();

      auto v_obs_robot = v_obs - v_robot;
      double overlap_amount = v_obs_robot.magnitude() - (obstacles_r + collision_radius);
      if (overlap_amount < 0) {
        collision = true;
        // There is a collision, we need to push robot out in this direction.
        auto push_amount = v_obs_robot.normalize() * overlap_amount;

        red_bot.SetBodyConfig({{v_robot += push_amount}, red_bot.GetBodyConfig().rotation()});
      }
    }
    return collision;
  }

  //! @brief Publish visualization markers for arena walls
  //! @param x_length Wall length in x
  //! @param y_length Wall length in y
  void PublishArenaWalls(double x_length, double y_length) {
    area_wall_publisher_ =
        create_publisher<visualization_msgs::msg::MarkerArray>("~/walls", transient_local_qos);

    visualization_msgs::msg::MarkerArray msg;

    visualization_msgs::msg::Marker wall_marker;
    wall_marker.type = wall_marker.CUBE;
    wall_marker.header.frame_id = kWorldFrame;
    wall_marker.header.stamp = get_clock()->now();
    wall_marker.scale.z = 0.25;
    wall_marker.scale.y = y_length;
    wall_marker.scale.x = x_length;
    wall_marker.color.r = 1.0;
    wall_marker.color.a = 1.0;
    wall_marker.pose.position.z = 0.25 / 2;
    visualization_msgs::msg::Marker x_plus_wall = wall_marker;
    x_plus_wall.scale.x = 0.01;
    x_plus_wall.pose.position.x = x_length / 2;
    x_plus_wall.id = 1;

    visualization_msgs::msg::Marker x_minus_wall = wall_marker;
    x_minus_wall.scale.x = 0.01;
    x_minus_wall.pose.position.x = -x_length / 2;
    x_minus_wall.id = 2;

    visualization_msgs::msg::Marker y_plus_wall = wall_marker;
    y_plus_wall.scale.y = 0.01;
    y_plus_wall.pose.position.y = y_length / 2;
    y_plus_wall.id = 3;

    visualization_msgs::msg::Marker y_minus_wall = wall_marker;
    y_minus_wall.scale.y = 0.01;
    y_minus_wall.pose.position.y = -y_length / 2;
    y_minus_wall.id = 4;

    msg.markers.push_back(x_plus_wall);
    msg.markers.push_back(x_minus_wall);
    msg.markers.push_back(y_plus_wall);
    msg.markers.push_back(y_minus_wall);
    area_wall_publisher_->publish(msg);

    std::cout << "Published markers" << std::endl;
  }

  //! @brief Publish vitilization markers for obstacles
  //! @param obstacles list of obstacle positions
  //! @param rad radius for all obstacle
  void PublishStaticObstacles(std::vector<visualization_msgs::msg::Marker> obstacles) {
    static_obstacle_publisher_ =
        create_publisher<visualization_msgs::msg::MarkerArray>("~/obstacles", transient_local_qos);

    visualization_msgs::msg::MarkerArray msg;

    size_t i = 0;
    for (const auto &obs : obstacles) {
      auto copy = obs;
      copy.header.frame_id = kWorldFrame;
      copy.header.stamp = get_clock()->now();
      copy.id = kStaticObstacleStartingID + (i++);
      copy.action = copy.ADD;
      msg.markers.push_back(copy);
    }
    static_obstacle_publisher_->publish(msg);
  }

  // Private Members

  // Constants

  constexpr static int32_t kStaticObstacleStartingID = 10;
  constexpr static int32_t kFakeSenorStartingID = 50;
  constexpr static size_t kRobotPathHistorySize = 10 ; // number of data points

  // Ros Params
  const std::chrono::nanoseconds update_period; // period for each cycle of update
  const double x0 = 0;                          // Init x location
  const double y0 = 0;                          // Init y location
  const double theta0 = 0;                      // Init theta location

  const int motor_cmd_max;
  const double motor_cmd_per_rad_sec;
  const double encoder_ticks_per_rad;
  const double collision_radius;
  turtlelib::DiffDrive red_bot;

  double obstacles_r; // Extra copy for quick access
  // The order of the array also defined their ID. so it matters they don't
  // change Obstacle constructed using input x_s and y_s Due to check needed on
  // x_s and y_s, not const
  const std::vector<visualization_msgs::msg::Marker> static_obstacles;
  
  const std::vector<std::pair<turtlelib::Point2D,turtlelib::Point2D>> arena_corners;
  // simulation only param
  const double input_noise;
  const double slip_fraction;
  const double max_range;
  const LaserParam sim_laser_param;
  // These are member variable
  std::normal_distribution<double> input_gauss_distribution;
  std::uniform_real_distribution<double> wheel_uniform_distribution;
  std::normal_distribution<double> basic_sensor_gauss_distribution;

  std::atomic<uint64_t> time_step_ = 0;
  nuturtlebot_msgs::msg::WheelCommands latest_wheel_cmd;
  std::deque<geometry_msgs::msg::PoseStamped> bot_path_history = std::deque<geometry_msgs::msg::PoseStamped>(kRobotPathHistorySize);
  geometry_msgs::msg::TransformStamped latest_bot_tf ; 

  // Ros objects
  rclcpp::TimerBase::SharedPtr main_timer_;
  rclcpp::TimerBase::SharedPtr fake_sensor_timer_;
  rclcpp::TimerBase::SharedPtr sim_laser_timer_;
  
  rclcpp::Service<std_srvs::srv::Empty>::SharedPtr reset_service_;
  rclcpp::Service<nusim::srv::Teleport>::SharedPtr teleport_service_;
  
  rclcpp::Publisher<std_msgs::msg::UInt64>::SharedPtr time_step_publisher_;
  rclcpp::Publisher<nuturtlebot_msgs::msg::SensorData>::SharedPtr red_sensor_publisher_;
  rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr fake_sensor_publisher_;
  rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr path_publisher_;
  rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr sim_laser_publisher_;
  std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster_;

  rclcpp::Subscription<nuturtlebot_msgs::msg::WheelCommands>::SharedPtr wheel_cmd_listener_;

  // The publisher need to be kept so the transient local message can still be
  // available later
  rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr area_wall_publisher_;
  rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr static_obstacle_publisher_;

};

//! @brief Main entry point for the nusim node
int main(int argc, char *argv[]) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<NuSim>());
  rclcpp::shutdown();
  return 0;
}
