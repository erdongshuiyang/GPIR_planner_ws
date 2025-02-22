/**
 * Copyright (C) 2022, RAM-LAB, Hong Kong University of Science and Technology
 * This file is part of GPIR (https://github.com/jchengai/gpir).
 * If you find this repo helpful, please cite the respective publication as
 * listed on the above website.
 */

#include "gp_planner/gp_planner.h"

#include <glog/logging.h>
#include <jsk_recognition_msgs/BoundingBox.h>
#include <visualization_msgs/MarkerArray.h>

#include "common/smoothing/osqp_spline2d_solver.h"
#include "common/utils/color_map.h"
#include "common/utils/io_utils.h"
#include "gp_planner/gp/gp_incremental_path_planner.h"
#include "gp_planner/st_plan/st_graph.h"
#include "planning_core/planning_common/planning_visual.h"

namespace planning {

void GPPlanner::Init() {
  ros::NodeHandle node;
  trajectory_pub_ =
      node.advertise<visualization_msgs::MarkerArray>("/gp_path", 1);
  critical_obstacle_pub_ =
      node.advertise<visualization_msgs::MarkerArray>("/critical_obstacles", 1);
  target_lane_pub_ = node.advertise<visualization_msgs::MarkerArray>(
      "/behavior_target_lane", 1);

  vehicle_param_ = VehicleInfo::Instance().vehicle_param();

  // 初始化数据收集器
  // if (!PlanningDataCollector::Initialize()) {
  //     LOG(ERROR) << "Failed to initialize data collector";
  // }

  path_data_pub_ = node.advertise<nav_msgs::Path>("/planning/path_data", 1);
  curvature_data_pub_ = node.advertise<std_msgs::Float64MultiArray>("/planning/curvature_data", 1);

}

void GPPlanner::PlanOnce(NavigationMap* navigation_map_) {
  const auto& ego_state = navigation_map_->ego_state();
  const auto& reference_lines = navigation_map_->reference_lines();
  const auto& virtual_obstacles = navigation_map_->virtual_obstacles();
  const auto& obstacles = navigation_map_->obstacles();
  reference_speed_ = navigation_map_->reference_speed();

  std::vector<std::pair<hdmap::LaneSegmentBehavior, common::Trajectory>>
      trajectory_candidate;

  int count = 0;
  for (const auto& reference_line : reference_lines) {
    trajectory_index_ = count;
    common::Trajectory trajectroy;
    if (!PlanWithGPIR(ego_state, reference_line, obstacles, virtual_obstacles,
                      navigation_map_->trajectory(), &trajectroy)) {
      LOG(ERROR) << "[Plan]: planning for "
                 << ToString(reference_line.behavior()) << " failed";
    } else {
      trajectory_candidate.emplace_back(
          std::make_pair(reference_line.behavior(), trajectroy));
    }
    ++count;
  }

  if (!trajectory_candidate.empty()) {
    *navigation_map_->mutable_trajectory() =
        trajectory_candidate.front().second;
    navigation_map_->SetPlannerLCFeedback(trajectory_candidate.front().first !=
                                          hdmap::LaneSegmentBehavior::kKeep);
    last_behavior_ = trajectory_candidate.front().first;
  } else {
    LOG(ERROR) << "[Plan]: cannot find valid trajectory, planning failed";
    navigation_map_->mutable_trajectory()->clear();
  }

  VisualizeTrajectory(trajectory_candidate);
  VisualizeTargetLane(reference_lines[std::min(
      reference_lines.size() - 1,
      reference_lines.size() - trajectory_candidate.size())]);
}

bool GPPlanner::ProcessObstacles(const std::vector<Obstacle>& raw_obstacles,
                                 const ReferenceLine& reference_line,
                                 std::vector<Obstacle>* cirtical_obstacles) {
  cirtical_obstacles->clear();
  ego_box_ = common::Box2D(state_.position, vehicle_param_.length,
                           vehicle_param_.width, state_.heading,
                           vehicle_param_.height);
  const auto& ref_lane_list = reference_line.lane_id_list();

  for (const auto& obstacle : raw_obstacles) {
    if (ego_box_.HasOverlapWith(obstacle.BoundingBox())) {
      LOG(ERROR) << "collision detected!";
      return false;
    }

    std::vector<int> intersect_lane;
    const auto& obstacle_lane_list = obstacle.lane_id_list();
    std::set_intersection(obstacle_lane_list.begin(), obstacle_lane_list.end(),
                          ref_lane_list.begin(), ref_lane_list.end(),
                          std::back_inserter(intersect_lane));

    auto proj = reference_line.GetRoughProjection(obstacle.state().position);

    bool outside_roi = proj.s < -30 || proj.s > reference_line.length() ||
                       std::fabs(proj.d) > lateral_critical_thereshold_;
    bool intersect_safe = intersect_lane.empty();

    if (outside_roi && intersect_safe) continue;
    if (!obstacle.is_static()) {
      cirtical_obstacles->emplace_back(obstacle);
    }
  }
  return true;
}

bool GPPlanner::PlanWithGPIR(
    const common::State& ego_state, const ReferenceLine& reference_line,
    const std::vector<Obstacle>& dynamic_agents,
    const std::vector<Eigen::Vector2d>& virtual_obstacles,
    const common::Trajectory& last_trajectory, common::Trajectory* trajectory) {

  // 创建数据收集器
  // std::vector<PathPlanningData> all_planning_data;
  // constexpr double kSampleInterval = 0.5;  // 采样间隔0.5米    

  common::Timer timer;
  time_consuption_ = TimeConsumption();

  const double length = reference_line.length();

  std::vector<Obstacle> cirtical_agents;
  ProcessObstacles(dynamic_agents, reference_line, &cirtical_agents);

  timer.Start();
  OccupancyMap occupancy_map({0, -5}, {std::ceil(length / 0.1) + 100, 100},
                             {0.1, 0.1});
  // OccupancyMap occupancy_map(
  //   {0, -250},           // 扩大原点范围
  //   {1500, 500},        // 扩大地图尺寸
  //   {0.1, 0.1});

  std::vector<double> obstacle_location_hint;
  for (const auto& point : virtual_obstacles) {
    // 将障碍物从笛卡尔坐标系投影到参考线上的Frenet坐标系
    auto proj = reference_line.GetProjection(point); // 得到(s,d)坐标
    occupancy_map.FillCircle(Eigen::Vector2d(proj.s, proj.d), 0.2); // FillCircle将障碍物表示为圆形，填充到栅格地图中
    // 记录障碍物在参考线上的s坐标(用于后续路径规划)
    obstacle_location_hint.emplace_back(proj.s);
  }
  // 1. 创建SDF对象
  auto sdf = std::make_shared<SignedDistanceField2D>(std::move(occupancy_map));
  // 2. 计算SDF
  sdf->UpdateSDF();
  time_consuption_.sdf = timer.EndThenReset();

  common::FrenetState frenet_state;
  reference_line.ToFrenetState(ego_state, &frenet_state);
  if (!last_trajectory.empty()) {
    auto last_state = last_trajectory.GetNearestState(ego_state.position);
    if (std::fabs(last_state.frenet_d[0] - frenet_state.d[0]) < 1.0) {
      reference_line.ToFrenetState(last_state, &frenet_state);
      frenet_state.d = last_state.frenet_d;
      // frenet_state.s = last_state.frenet_s;
    }
  }

  GPPath gp_path;
  GPIncrementalPathPlanner gp_path_planner(sdf);

   // 在更新不确定性之前输出日志
   LOG(INFO) << "Before setting uncertainty to planner";
   LOG(INFO) << "Static obstacle uncertainty:\n" 
              << static_obstacle_uncertainty_.position_covariance;

   // 将不确定性传递给路径规划器
  gp_path_planner.SetStaticObstacleUncertainty(static_obstacle_uncertainty_);

  // 设置后再次输出
  LOG(INFO) << "After setting uncertainty to planner ";

  if (!gp_path_planner.GenerateInitialGPPath(reference_line, frenet_state, 100,
                                             obstacle_location_hint,
                                             dynamic_agents, 
                                             &gp_path)) {
    LOG(ERROR) << "[GPPlanner]: fail to generate initla path";
    return false;
  }

  PublishPlanningData(gp_path);  // 发布初始路径数据

  // 在初始路径生成后收集第一组数据
  // PathPlanningData initial_data;
  // initial_data.iteration_count = 0;
  // PlanningDataCollector::CollectPathData(gp_path, kSampleInterval, &initial_data);
  // all_planning_data.push_back(initial_data);

  time_consuption_.init_path = timer.EndThenReset();

  reference_line.ToFrenetState(ego_state, &frenet_state);
  // 速度规划初始化
  StGraph st_graph(frenet_state.s);
  st_graph.SetReferenceSpeed(reference_speed_);
  // 构建ST图
  st_graph.BuildStGraph(cirtical_agents, gp_path);
  // 速度搜索
  if (!st_graph.SearchWithLocalTruncation(13, nullptr)) {
    LOG(ERROR) << "[StGraph]: fail to find initial speed profile";
    return false;
  }
  // 速度优化
  if (!st_graph.GenerateInitialSpeedProfile(gp_path)) {
    LOG(ERROR) << "[StGraph]: fail to optimize inital speed profile";
    return false;
  }
  time_consuption_.init_st = timer.EndThenReset();

  int iter_count = 0;
  vector_Eigen3d invalid_lat_frenet_s;
  // 可行性检查与迭代优化
  while (trajectory_index_ < 1 &&
         !st_graph.IsTrajectoryFeasible(gp_path, &invalid_lat_frenet_s)) {
    gp_path_planner.UpdateGPPath(reference_line, invalid_lat_frenet_s, dynamic_agents, 
                                 &gp_path);
    // gp_path_planner.UpdateGPPathNonIncremental(reference_line,
    //                                            invalid_lat_frenet_s,
    //                                            &gp_path);
    // st_graph.UpdateSpeedProfile(gp_path);

    // 在每次路径更新后收集数据
    // PathPlanningData iter_data;
    // iter_data.iteration_count = iter_count + 1;
    // PlanningDataCollector::CollectPathData(gp_path, kSampleInterval, &iter_data);
    // all_planning_data.push_back(iter_data);  

    // 在每次路径更新后发布数据
    PublishPlanningData(gp_path);  // 发布迭代优化后的路径数据


    if (iter_count++ == max_iter) {
      LOG(WARNING) << "[GPIR]: reach maximum iterations";
      break;
    }
  }
  if (iter_count != 0) {
    time_consuption_.refinement = timer.EndThenReset();
  }

  // 在生成最终轨迹之前保存数据
  // static int scenario_count = 0;
  // std::string data_dir = "/home/erdong2004/GPIR_planner_ws/src/gpir/data";
  // std::string filename = data_dir + "path_planning_data_" + 
  //                       std::to_string(scenario_count++) + ".csv";
  // 直接调用SaveToCSV，让类内部处理文件名生成
  // PlanningDataCollector::SaveToCSV(all_planning_data);

  // 生成最终轨迹
  st_graph.GenerateTrajectory(reference_line, gp_path, trajectory);

  // 在最终轨迹生成后发布一次数据
  PublishPlanningData(gp_path);  // 发布最终路径数据

  VisualizeCriticalObstacle(cirtical_agents);
  return true;
}

void GPPlanner::VisualizeTrajectory(
    const std::vector<std::pair<hdmap::LaneSegmentBehavior,
                                common::Trajectory>>& trajectory_candidates) {
  visualization_msgs::MarkerArray markers;
  visualization_msgs::Marker shape;

  constexpr double sample_dis = 3;
  int id_count = 0;
  double alpha = 1.0;
  common::Color line_color, node_color;
  for (int i = 0; i < trajectory_candidates.size(); ++i) {
    visualization_msgs::Marker line, node;
    if (i == 0) {
      alpha = 1.0;
      line_color = common::Color::kOrangeRed;
      node_color = common::Color::kBlue;
    } else {
      alpha = 0.5;
      line_color = common::Color::kGrey;
      node_color = common::Color::kBlack;
    }

    PlanningVisual::FillHeader(&line.header);
    PlanningVisual::SetScaleAndColor({0.15, 0.15, 0.15}, line_color, &line,
                                     alpha);
    line.id = id_count++;
    line.type = visualization_msgs::Marker::LINE_STRIP;
    line.action = visualization_msgs::Marker::MODIFY;
    line.pose.orientation.w = 1.0;
    line.lifetime = ros::Duration(0.15);

    PlanningVisual::FillHeader(&node.header);
    PlanningVisual::SetScaleAndColor({0.5, 0.5, 0.1}, node_color, &node, alpha);
    node.type = visualization_msgs::Marker::CYLINDER;
    node.action = visualization_msgs::Marker::MODIFY;
    node.pose.orientation.w = 1;
    node.pose.orientation.w = 1.0;
    node.lifetime = ros::Duration(0.15);

    if (i == 0) {
      PlanningVisual::FillHeader(&shape.header);
      PlanningVisual::SetScaleAndColor({2.1, 0, 0}, common::kGold, &shape, 0.2);
      shape.id = id_count++;
      shape.type = visualization_msgs::Marker::LINE_STRIP;
      shape.action = visualization_msgs::Marker::MODIFY;
      shape.pose.orientation.w = 1;
      shape.lifetime = ros::Duration(0.15);
    }

    geometry_msgs::Point point;
    double start_s = trajectory_candidates[i].second.front().s;
    for (const auto& traj_point : trajectory_candidates[i].second) {
      if (traj_point.s - start_s > sample_dis) {
        start_s = traj_point.s;
        node.id = id_count++;
        node.pose.position.x = traj_point.position.x();
        node.pose.position.y = traj_point.position.y();
        node.pose.position.z = 0.2;
        markers.markers.push_back(node);
      }
      point.x = traj_point.position.x();
      point.y = traj_point.position.y();
      point.z = 0.2;
      line.points.push_back(point);
      if (i == 0) {
        shape.points.push_back(point);
      }
    }
    markers.markers.push_back(line);
  }
  markers.markers.push_back(shape);
  trajectory_pub_.publish(markers);
}

void GPPlanner::VisualizeCriticalObstacle(
    const std::vector<Obstacle>& critical_obstacles) {
  visualization_msgs::MarkerArray markers;
  visualization_msgs::Marker marker;
  marker.lifetime = ros::Duration(0.15);

  geometry_msgs::Point p;
  int id_count = 0;
  for (const auto& obs : critical_obstacles) {
    PlanningVisual::Get2DBoxMarker(obs.state().position, obs.width() + 0.3,
                                   obs.length() + 0.3, obs.state().heading,
                                   common::Color::kRed, {0.3, 0, 0}, &marker);
    marker.id = id_count++;
    markers.markers.emplace_back(marker);
  }

  critical_obstacle_pub_.publish(markers);
}

void GPPlanner::VisualizeTargetLane(const ReferenceLine& reference_line) {
  visualization_msgs::MarkerArray markers;
  visualization_msgs::Marker marker;
  marker.header.frame_id = "map";
  marker.header.stamp = ros::Time::now();
  marker.type = visualization_msgs::Marker::LINE_LIST;
  marker.action = visualization_msgs::Marker::MODIFY;
  marker.pose.orientation.w = 1.0;

  constexpr double step_length = 5.0;
  constexpr double angle = M_PI_4;
  double length = 0.75 / std::sin(angle);

  auto rotate_vector = [&length](const Eigen::Vector3d& base,
                                 const double angle) {
    geometry_msgs::Point point;
    auto rotated_vector =
        base.topRows(2) + length * Eigen::Vector2d(std::cos(base(2) + angle),
                                                   std::sin(base(2) + angle));
    point.x = rotated_vector.x();
    point.y = rotated_vector.y();
    return point;
  };

  marker.id = 0;
  for (double s = 0; s < reference_line.length(); s += step_length) {
    auto bh = reference_line.behavior();
    if (bh == hdmap::LaneSegmentBehavior::kKeep) {
      PlanningVisual::SetScaleAndColor({0.4, 0, 0}, common::Color::kGreen,
                                       &marker);
    } else {
      PlanningVisual::SetScaleAndColor({0.4, 0, 0}, common::Color::kMagenta,
                                       &marker);
    }
    auto se2 = reference_line.GetSE2(s);
    geometry_msgs::Point origin;
    origin.x = se2.x();
    origin.y = se2.y();
    auto left = rotate_vector(se2, M_PI - angle);
    marker.points.push_back(origin);
    marker.points.push_back(left);
    auto right = rotate_vector(se2, -M_PI + angle);
    marker.points.push_back(origin);
    marker.points.push_back(right);
  }
  markers.markers.push_back(marker);

  target_lane_pub_.publish(markers);
}


void GPPlanner::PublishPlanningData(const GPPath& path) {
  // 发布路径数据
  nav_msgs::Path path_msg;
  path_msg.header.stamp = ros::Time::now();
  path_msg.header.frame_id = "map";
  
  // 发布曲率数据
  std_msgs::Float64MultiArray curv_msg;
  
  constexpr double kSampleInterval = 0.5;
  for(double s = path.start_s(); s <= path.MaximumArcLength(); s += kSampleInterval) {
      common::State state;
      path.GetState(s, &state);
      
      // 添加路径点
      geometry_msgs::PoseStamped pose;
      pose.pose.position.x = state.position.x();
      pose.pose.position.y = state.position.y();
      path_msg.poses.push_back(pose);
      
      // 添加曲率值
      curv_msg.data.push_back(path.GetCurvature(s));
  }
  
  path_data_pub_.publish(path_msg);
  curvature_data_pub_.publish(curv_msg);
}

}  // namespace planning
