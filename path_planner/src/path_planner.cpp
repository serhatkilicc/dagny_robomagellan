/* path_planner.cpp
 *
 * A ROS path planner node for my robot.
 * subscribes to the robot's current position and the current goal, and
 * uses this information to plan and execute a path to the goal
 *
 * All distances in meters
 *
 * Author: Austin Hendrix
 *
 * TODO:
 *  Find synchronization bug with goal_list
 *  Add swtich to turn cone tracking on and off
 *  Add parameters
 *  Add dynamic_reconfigure
 *  Better detection of being stuck
 *  Sonar integration
 *  Wheel slip detection
 *  Switch to publish local costmap
 *  Downsample and/or crop local costmap before publishing
 * 
 * Long-term:
 *  Take lessons learned here and port to navigation stack
 */

#include <math.h>
#include <assert.h>
#include <stdio.h>

#include <set>
#include <list>
#include <map>
#include <vector>

#include <boost/foreach.hpp>

#include <ros/ros.h>
#include <tf/tf.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <nav_msgs/Odometry.h>
#include <nav_msgs/OccupancyGrid.h>
#include <nav_msgs/Path.h>
#include <sensor_msgs/LaserScan.h>
#include <sensor_msgs/NavSatFix.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Float32.h>
#include <visualization_msgs/Marker.h>
#include <geometry_msgs/PointStamped.h>

#include <dynamic_reconfigure/server.h>
#include <path_planner/PathPlannerConfig.h>

using namespace std;

// minimum turning radius (m)
double min_radius = 0.695;
// maximum planned radius (m)
//double max_radius = 10.0;
double max_radius = 4.0;
// how close we want to get to our goal before we're "there" (m)
double goal_err = 0.3;
// how close we are before we switch to cone mode (m)
double cone_dist = 6.0;


// map resolution, in meters per pixel
// TODO: convert to parameter
#define MAP_RES 0.10
// map size, in cells
// TODO: convert to parameter
#define MAP_SIZE 5000

// speed for path traversal (m/s)
double max_speed = 1.5;
double min_speed = 0.1;
double planner_lookahead = 4.0;
// TODO: use proper units
double max_accel = 0.3;

// planner timeouts
double backup_time = 10.0;
double backup_dist = 1.0;
double backup_radius = 0.0;
double stuck_timeout = 2.0;

// cone-tracking values
double cone_timeout = 1.0;
double cone_speed = 0.4;

// enable/disable for cone mode
bool track_cones = false;

// planner active
bool active = false;

std::string position_frame;

// types, to make life easier
struct loc {
   // x, y, pose: the position and direction of the robot
   double x;
   double y;
   double pose;

   loc() : x(0.0), y(0.0), pose(0.0) {}

   loc(const geometry_msgs::PointStamped &point) :
     x(point.point.x), y(point.point.y), pose(0.0) {}
};

struct path {
   double speed;
   double radius;
};

// the local obstacle map
// fixed dimensions: 100m by 100m, centered about the associated center point
// FIXME: replace this with calls to the global_map and SLAM
typedef int8_t map_type;
map_type * map_data;

// get the value of the local obstacle map at (x, y)
//  return 0 for any point not within the obstacle map
inline map_type map_get(double x, double y) {
   int i = round(x/MAP_RES) + MAP_SIZE/2;
   int j = round(y/MAP_RES) + MAP_SIZE/2;
   if( i >= 0 && i < MAP_SIZE && j >= 0 && j < MAP_SIZE ) {
      return map_data[(i * MAP_SIZE) + j];
   } else {
      return 0;
   }
}

// get the value of the local obstacle map at (x, y)
//  return 0 for any point not within the obstacle map
inline void map_set(double x, double y, map_type v) {
   int i = round(x/MAP_RES) + MAP_SIZE/2;
   int j = round(y/MAP_RES) + MAP_SIZE/2;
   if( i >= 0 && i < MAP_SIZE && j >= 0 && j < MAP_SIZE ) {
      map_data[(i * MAP_SIZE) + j] = v;
   }
}

// test if we have a collision at a particular point
bool test_collision(loc here) {
   return map_get(here.x, here.y) != 0;
}

// test an arc start at start with radius r for length l
bool test_arc(loc start, double r, double l) {
   if( r != 0.0 ) {
      // normal case; traverse an arc
      double center_x, center_y, theta;
      theta = start.pose - M_PI/2;
      center_x = start.x + r * cos(start.pose + M_PI/2);
      center_y = start.y + r * sin(start.pose + M_PI/2);

      // traverse along the arc until we hit something
      for( double dist = 0; dist < l; dist += MAP_RES/2.0 ) {
         loc h;
         h.x = r * cos(theta + dist / r) + center_x;
         h.y = r * sin(theta + dist / r) + center_y;
         if( test_collision(h) ) {
            //ROS_WARN("Obstacle at %lf", dist);
            return false;
         }
      }
   } else {
      // degenerate case; traverse a line
      for( double dist = 0; dist < l; dist += MAP_RES/2.0 ) {
         loc h;
         h.x = start.x + dist*cos(start.pose);
         h.y = start.y + dist*sin(start.pose);
         if( test_collision(h) ) {
            //ROS_WARN("Obstacle at %lf", dist);
            return false;
         }
      }
   }
   return true;
}

nav_msgs::Path arcToPath(loc start, double r, double l) {
   nav_msgs::Path p;
   p.header.frame_id = position_frame;
   if( r != 0.0 ) {
      // normal case; traverse an arc
      double center_x, center_y, theta;
      theta = start.pose - M_PI/2;
      center_x = start.x + r * cos(start.pose + M_PI/2);
      center_y = start.y + r * sin(start.pose + M_PI/2);

      // traverse along the arc until we hit something
      for( double dist = 0; dist < l; dist += MAP_RES/2.0 ) {
         double x = r * cos(theta + dist / r) + center_x;
         double y = r * sin(theta + dist / r) + center_y;
         geometry_msgs::PoseStamped pose;
         pose.header.frame_id = position_frame;
         pose.pose.position.x = x;
         pose.pose.position.y = y;
         p.poses.push_back(pose);
         //ROS_INFO("Map at %lf, %lf: %d", x, y, map_get(x, y));
      }
   } else {
      // degenerate case; traverse a line
      for( double dist = 0; dist < l; dist += MAP_RES/2.0 ) {
         geometry_msgs::PoseStamped pose;
         pose.header.frame_id = position_frame;
         pose.pose.position.x = start.x + dist*cos(start.pose);
         pose.pose.position.y = start.y + dist*sin(start.pose);
         p.poses.push_back(pose);
      }
   }
   return p;
}

loc arc_end(loc start, double r, double l) {
   loc ret;
   if( r != 0.0 ) {
      // normal case; traverse an arc
      double center_x, center_y, theta;
      theta = start.pose - M_PI/2;
      center_x = start.x + r * cos(start.pose + M_PI/2);
      center_y = start.y + r * sin(start.pose + M_PI/2);

      ret.x = center_x + r*cos(theta + l / r);
      ret.y = center_y + r*sin(theta + l / r);
      ret.pose = theta + l / r;
   } else {
      // degenerate case: straight line
      ret.x = start.x + l*cos(start.pose);
      ret.y = start.y + l*sin(start.pose);
      ret.pose = start.pose;
   }
   return ret;
}

#define dist(a, b) hypot(a.x - b.x, a.y - b.y)

ros::Publisher path_pub;
ros::Publisher done_pub;
ros::Time done_time;

enum pstate {
   BACKING, FORWARD, CONE
};

pstate planner_state;

ros::Time planner_timeout;
loc backup_pose;

visualization_msgs::Marker cones;

float cone;
ros::Time cone_time;

bool bump = false;

loc pattern_center;

/* plan a path from start to end
 *  TODO: find a clear path all the way to the edge of the map or the goal, 
 *   whichever is closer
 *  TODO: try different arc lengths
 *  TODO: add state backing up support
 */
path plan_path(loc start, loc end) {
   /*
   ROS_INFO("Searching for path from (% 5.2lf, % 5.2lf) to (% 5.2lf, % 5.2lf)",
         start.x, start.y, end.x, end.y);
         */
   path p;
   double d = dist(start, end);
   // TODO: don't go into cone tracking immediately on startup
   if( track_cones && d < cone_dist && planner_state == FORWARD ) {
      planner_state = CONE;
      pattern_center = start;
      planner_timeout = ros::Time::now();
      ROS_INFO("Starting cone tracking");
   }

   switch(planner_state) {
      case BACKING:
         p.speed = -2.0 * min_speed;
         p.radius = backup_radius;
         if( (ros::Time::now() - planner_timeout).toSec() > backup_time ) {
            planner_state = FORWARD;
            planner_timeout.sec = 0;
         }
         if( dist(start, backup_pose) > backup_dist ) {
            planner_state = FORWARD;
            planner_timeout.sec = 0; // TODO: figure out if we still need to reset this here.
         }
         break;
      case CONE:
         {
            // find nearest cone
            /*
            if( cones.points.size() > 0 ) {
               geometry_msgs::Point cone = cones.points.front();
               double cone_d = hypot(cone.x - start.x, cone.y - start.y);
               BOOST_FOREACH(geometry_msgs::Point p, cones.points) {
                  double d = hypot(p.x - start.x, p.y - start.y);
                  if( d < cone_d ) {
                     cone = p;
                     cone_d = d;
                  }
               }
               p.speed = min_speed * 4.0;
               p.radius = 0;
               double cone_angle = atan2(cone.y - start.y, cone.x - start.x);
               double turn_angle = cone_angle - start.pose;
               // normalize turn angle
               while( turn_angle > M_PI ) turn_angle -= 2*M_PI;
               while( turn_angle < -M_PI ) turn_angle += 2 * M_PI;

               ROS_INFO("Angle to cone %lf", turn_angle);
               if( turn_angle > 0.1 ) {
                  p.radius = min_radius;
                  ROS_INFO("Cone left");
               }
               if( turn_angle < -0.1 ) {
                  p.radius = -min_radius;
                  ROS_INFO("Cone right");
               }
               */
            p.speed = cone_speed;
            if( (ros::Time::now() - cone_time).toSec() < cone_timeout ) {
               p.radius = (p.speed / (cone * 1.4));
            } else {
               ROS_INFO("No cones");
               // if we don't see any cones, drive in spirals
               //  a figure-8 pattern is probably best, but it's also hard

               p.radius = 2.0;
               /*
               p.radius = min_radius + 
                  (2.0*cone_timeout)/(ros::Time::now() - cone_time).toSec();
                  */
               /*
               p.radius = 0;
               double dx = start.x - pattern_center.x;
               //double dy = start.y - pattern_center.y;
               double search_radius = 2.0;
               if( dx > search_radius*sqrt(0.5) ) {
                  ROS_INFO("Figure 8: right of center");
                  // turn right
                  if( start.pose < M_PI/2.0 || start.pose > 3.0*M_PI/4.0 ) {
                     ROS_INFO("Figure 8: turning right");
                     p.radius = -search_radius;
                  } else {
                     ROS_INFO("Figure 8: turning left");
                     p.radius = search_radius;
                  }
               } else if( dx < -search_radius*sqrt(0.5) ) {
                  ROS_INFO("Figure 8: left of center");
                  // turn left
                  if( start.pose > M_PI/2.0 || start.pose < M_PI/4.0 ) {
                     ROS_INFO("Figure 8: turning left");
                     p.radius = search_radius;
                  } else {
                     ROS_INFO("Figure 8: turning right");
                     p.radius = -search_radius;
                  }
               } else {
                  ROS_INFO("Figure 8: within X region");
               }
               */
            }
            
            // if we hit the cone, back up and keep going
            if( bump ) {
               planner_timeout = ros::Time::now();
               backup_pose = start;
               planner_state = BACKING;
               p.speed = 0;
               p.radius = 0;

               ROS_INFO("Cone hit");
               active = false;

               std_msgs::Bool res;
               res.data = true;
               done_pub.publish(res);
            }
            if( planner_timeout + ros::Duration(60.0) < ros::Time::now() ) {
               planner_state = FORWARD;
               p.speed = 0;
               p.radius = 0;

               ROS_INFO("Cone tracking timed out");
               active = false;

               std_msgs::Bool res;
               res.data = false;
               done_pub.publish(res);
            }
         }
         break;
      case FORWARD:
         // don't plan if we're at the goal
         if( d < goal_err ) {
            p.speed = 0;
            p.radius = 0;
            ROS_INFO("Goal reached");
            active = false;
            std_msgs::Bool res;
            res.data = true;
            if( (ros::Time::now() - done_time).toSec() > 0.5 ) {
               done_time = ros::Time::now();
               done_pub.publish(res);
            }
            break;
         }
         double theta = atan2(end.y - start.y, end.x - start.x);
         //ROS_INFO("Angle to goal: %lf", theta);

         double traverse_dist = min(d, planner_lookahead);
         double speed = min(max_speed, 
               max_speed * (2.0 * traverse_dist / planner_lookahead));
         if( speed < min_speed) speed = min_speed;
         //ROS_INFO("Traverse distance %lf, speed %lf", traverse_dist, speed);

         // radius > 0 -> left
         double radius = 0;

         // test tangent arc through the goal point:
         double alpha = 2.0 * (theta - start.pose); // arc angle
         // normalize (theta - start.pose)
         while( alpha > 2.0*M_PI )  alpha -= M_PI * 4.0;
         while( alpha < -2.0*M_PI ) alpha += M_PI * 4.0;
         double arc_len = alpha; // grab inner angle before normalization

         // normalize while maintaining sign
         if( alpha >  M_PI ) alpha =  M_PI * 2.0 - alpha;
         if( alpha < -M_PI ) alpha = -M_PI * 2.0 - alpha;

         double beta = (M_PI - fabs(alpha)) / 2.0; // internal angle
         radius = d * sin(beta) / sin(alpha);
         // if we want to turn around, use minimum radius
         if( fabs(arc_len) > M_PI ) {
            if( radius > 0 ) {
               radius = min_radius;
            } else {
               radius = -min_radius;
            }
         }
         arc_len = arc_len * radius;

         // if our turn radius is below our minimum radius, go straight
         if( fabs(radius) < min_radius ) {
            ROS_INFO("Tangent arc radius too small; looping around. %lf", radius);
            radius = 0;
            // we should go forward by our minimum radius, and then loop around
            arc_len = min_radius;
         }

         // don't plan huge sweeping curves; choose max radius
         radius = min(radius,  max_radius);
         radius = max(radius, -max_radius);

         arc_len = min(arc_len, planner_lookahead);

         if( !test_arc(start, radius, arc_len) ) {
            ROS_WARN("Tangent arc failed");

            // test various radii for traverse_dist
            list<double> arcs;
            if( test_arc(start, 0, traverse_dist) ) {
               arcs.push_back(0);
            }
            // 1, 2, 4, 8 * min_radius
            for( int i=1; i<9; i *= 2 ) {
               // traverse at most a quarter turn
               // TODO: try various traverse distances
               //  followed by a straight path to the edge of the map
               double d = min(traverse_dist, min_radius * i * M_PI / 2);
               if( test_arc(start, min_radius*i, d) ) {
                  arcs.push_back(min_radius*i);
               }
               if( test_arc(start, -min_radius*i, d) ) {
                  arcs.push_back(-min_radius*i);
               }
            }
            if( arcs.size() == 0 ) {
               ROS_WARN("No valid forward paths found");
               speed = 0;
               radius = 0;
               if( planner_timeout.sec != 0 ) {
                  if( (ros::Time::now() -  planner_timeout).toSec() > 
                        stuck_timeout ) {
                     planner_state = BACKING;
                     if( alpha > 0 ) {
                        backup_radius = -min_radius;
                     } else {
                        backup_radius = min_radius;
                     }
                     backup_pose = start;
                     planner_timeout = ros::Time::now();
                     ROS_WARN("Robot stuck; backing up");
                  }
               } else {
                  planner_timeout = ros::Time::now();
               }
            } else {
               // choose arc that gets us closest to goal
               //ROS_INFO("%zd forward paths found", arcs.size());
               double best_r = arcs.front();
               double l = min(traverse_dist, best_r * M_PI / 2);
               loc e = arc_end(start, best_r, l);
               double min_dist = dist(e, end);

               BOOST_FOREACH(double r, arcs) {
                  l = min(traverse_dist, r * M_PI / 2);
                  e = arc_end(start, r, l);
                  double d = dist(e, end);
                  if( d < min_dist ) {
                     best_r = r;
                     min_dist = d;
                  }
               }
               radius = best_r;
               arc_len = fabs(best_r * M_PI / 2);
               if( best_r == 0.0 ) arc_len = traverse_dist;
               speed = min(max_speed, max_speed * (2.0 * arc_len / planner_lookahead));
               nav_msgs::Path p = arcToPath(start, best_r, 
                     min(traverse_dist, arc_len));
               path_pub.publish(p);
               // reset backup timer
               planner_timeout.sec = 0;
            }
         } else {
            nav_msgs::Path p = arcToPath(start, radius, arc_len);
            path_pub.publish(p);
            // reset backup timer
            planner_timeout.sec = 0;
         }
         ROS_INFO("Traverse distance %lf, speed %lf", traverse_dist, speed);
         p.radius = radius;
         p.speed = speed;
         break;
   }

   return p;
}

// publisher for publishing movement commands
ros::Publisher cmd_pub;
// publisher for map
ros::Publisher map_pub;

bool path_valid = false;
geometry_msgs::PointStamped goal_msg;

// frame transform bits. Keep track of the position frame id and the
// tf2 buffer
tf2_ros::Buffer tf2_buffer;

void goalCallback(const geometry_msgs::PointStamped::ConstPtr & msg) {
  goal_msg = *msg;
  active = true;
}

// the last location we were at.
//  used as the center point for our local map
loc last_loc;
geometry_msgs::Pose last_pose;
   
void positionCallback(const nav_msgs::Odometry::ConstPtr & msg) {
   loc here;
   here.x = msg->pose.pose.position.x;
   here.y = msg->pose.pose.position.y;
   here.pose = tf::getYaw(msg->pose.pose.orientation);

   last_loc = here;
   last_pose = msg->pose.pose;
   std::string pose_frame = msg->header.frame_id;
   position_frame = pose_frame;

   if( pose_frame != goal_msg.header.frame_id ) {
     geometry_msgs::PointStamped tmp_goal;
     std::string tf_err;
     if( tf2_buffer.canTransform(pose_frame, goal_msg.header.frame_id,
           goal_msg.header.stamp, &tf_err) ) {
       tf2_buffer.transform(goal_msg, tmp_goal, pose_frame);
       goal_msg = tmp_goal;
     } else {
       ROS_ERROR("Cannot transform goal from %s frame to %s frame: %s",
           goal_msg.header.frame_id.c_str(), pose_frame.c_str(),
           tf_err.c_str());
       return;
     }
   }

   loc goal(goal_msg);

   if( active ) {
      geometry_msgs::Twist cmd;

      path p = plan_path(here, goal);
      double radius = p.radius;
      double speed = p.speed;
      double current_speed = msg->twist.twist.linear.x;

      // limit acceleration
      if( speed > 0 ) {
         if( current_speed > 0 ) {
            speed = min(speed, current_speed + max_accel);
         } else {
            speed = max_accel;
         }
      } else if( speed < 0 ) {
         if( current_speed < 0 ) {
            speed = max(speed, current_speed - max_accel);
         } else {
            speed = -max_accel;
         }
      }
      // no limit on deceleration


      // steering radius = linear / angular
      // angular = linear / radius
      if( radius != 0.0 ) {
         cmd.angular.z = speed / radius;
      } else {
         cmd.angular.z = 0;
      }

      //ROS_INFO("Target radius: %lf, angular: %lf", radius, cmd.angular.z);

      cmd.linear.x = speed;
      ROS_INFO("Target speed: %lf", speed);
      /*
      if( dist(here, goal) > goal_err ) {
         cmd.linear.x = speed;
      } else {
         cmd.linear.x = 0;
         cmd.angular.z = 0;
      }
      */
      cmd_pub.publish(cmd);
   } else {
      geometry_msgs::Twist cmd;
      cmd_pub.publish(cmd);
   }
}

#define LOCAL_MAP_SIZE 150
#define LASER_OFFSET 0.26

void laserCallback(const sensor_msgs::LaserScan::ConstPtr & msg) {
   //map_center_x = last_loc.x;
   //map_center_y = last_loc.y;
   loc here = last_loc;

   double theta_base = last_loc.pose;

   double theta = theta_base + msg->angle_min;
   double x;
   double y;

   double offset_x = modf(here.x/MAP_RES, &x)*MAP_RES; // don't care about x
   double offset_y = modf(here.y/MAP_RES, &y)*MAP_RES; // don't care about y

   // manual laser transform. I'm a horrible person
   offset_x += LASER_OFFSET * cos(theta_base);
   offset_y += LASER_OFFSET * sin(theta_base);

   map_type * local_map = (map_type*)malloc(LOCAL_MAP_SIZE*LOCAL_MAP_SIZE*
         sizeof(map_type));
   memset(local_map, 0, LOCAL_MAP_SIZE*LOCAL_MAP_SIZE*sizeof(map_type));
   int j, k;
   
   // build a local map and merge it with the global map

   // for each laser scan point, raytrace
   for( unsigned int i=0; i<msg->ranges.size(); i++, 
         theta += msg->angle_increment ) {
      double d;
      double r = msg->ranges[i];
      int status = 1;
      if( r < msg->range_min ) {
         // pull status codes out of laser data according to SCIP1.1
         if( r == 0.0 ) {
            r = 22.0; // raytrace out to 22m
         } else if( 0.0055 < r && r < 0.0065 ) {
            r = 5.7;
         } else if( 0.0155 < r && r < 0.0165 ) {
            r = 5.0;
         } else {
            status = 0;
         }
      }
      if( status ) {
         for( d=0; d<r; d += MAP_RES/2.0 ) {
            x = offset_x + d*cos(theta);
            y = offset_y + d*sin(theta);

            j = round(x/MAP_RES) + LOCAL_MAP_SIZE/2;
            k = round(y/MAP_RES) + LOCAL_MAP_SIZE/2;
            if( j > 0 && k > 0 && j < LOCAL_MAP_SIZE && k < LOCAL_MAP_SIZE ) {
               local_map[j*LOCAL_MAP_SIZE + k] = -1;
            } else {
               break; // if we step outside the local map bounds, we're done
            }
         }
      }
   }
   
   // mark obstacles
   theta = theta_base + msg->angle_min;
   for( unsigned int i=0; i<msg->ranges.size(); i++, 
         theta += msg->angle_increment ) {
      if( msg->ranges[i] > msg->range_min ) {
         x = offset_x + msg->ranges[i]*cos(theta);
         y = offset_y + msg->ranges[i]*sin(theta);

         j = round(x/MAP_RES) + LOCAL_MAP_SIZE/2;
         k = round(y/MAP_RES) + LOCAL_MAP_SIZE/2;
         if( j > 0 && k > 0 && j < LOCAL_MAP_SIZE && k < LOCAL_MAP_SIZE ) {
            local_map[j*LOCAL_MAP_SIZE + k] = 1;
         }
      }
   }

   // grow obstacles by radius of robot; makes collision-testing easier
   // order: O(n^2 * 12)
   for( int r=1; r<(0.4/MAP_RES); r++ ) {
      for( int i=0; i<LOCAL_MAP_SIZE; i++ ) {
         for( int j=0; j<LOCAL_MAP_SIZE; j++ ) {
            if( local_map[i*LOCAL_MAP_SIZE + j] <= 0 ) {
               if( i > 0   && local_map[(i-1)*LOCAL_MAP_SIZE + j  ] == r ) 
                  local_map[i*LOCAL_MAP_SIZE + j] = r+1;
               if( j > 0   && local_map[i*LOCAL_MAP_SIZE + j-1] == r )
                  local_map[i*LOCAL_MAP_SIZE + j] = r+1;
               if( i < LOCAL_MAP_SIZE && 
                     local_map[(i+1)*LOCAL_MAP_SIZE + j  ] == r )
                  local_map[i*LOCAL_MAP_SIZE + j] = r+1;
               if( j < LOCAL_MAP_SIZE && 
                     local_map[i*LOCAL_MAP_SIZE + j+1] == r ) 
                  local_map[i*LOCAL_MAP_SIZE + j] = r+1;
            }
         }
      }
   }

   // merge into global map
   offset_x = round(here.x/MAP_RES)*MAP_RES;
   offset_y = round(here.y/MAP_RES)*MAP_RES;
   for( int i=0; i<LOCAL_MAP_SIZE; i++ ) {
      for( int j=0; j<LOCAL_MAP_SIZE; j++ ) {
         x = (i - LOCAL_MAP_SIZE/2) * MAP_RES + offset_x;
         y = (j - LOCAL_MAP_SIZE/2) * MAP_RES + offset_y;
         map_type tmp = 0;
         tmp += local_map[i*LOCAL_MAP_SIZE + j];
         if( tmp > 0 ) tmp = 2; // flatten obstacle radius
         tmp += map_get(x, y);
         if( tmp > 4 ) tmp = 4;
         if( tmp < 0 ) tmp = 0;
         map_set(x, y, tmp);
      }
   }

   // clear out base footprint
   theta = here.pose;
   for( double bx = -0.16; bx <= 0.16; bx += MAP_RES/2.0 ) {
      for( double by = -0.17; by < 0.45; by += MAP_RES/2.0 ) {
         x = bx*cos(theta) + here.x;
         y = by*sin(theta) + here.y;
         map_set(x, y, 0);
      }
   }

   free(local_map);

   /*
   static int div = 0;
   ++div;
   if( div % 20 == 0 ) {
      // publish map
      nav_msgs::OccupancyGrid map;
      map.header = msg->header;
      map.header.frame_id = "odom";
      map.info.resolution = MAP_RES;
      map.info.width = MAP_SIZE;
      map.info.height = MAP_SIZE; 
      map.info.origin.position.x = - (MAP_SIZE * MAP_RES) / 2.0;
      map.info.origin.position.y = - (MAP_SIZE * MAP_RES) / 2.0;
      map.info.origin.orientation.w = 1.0;
      for( int i=0; i<MAP_SIZE; i++ ) {
         for( int j=0; j<MAP_SIZE; j++ ) {
            map.data.push_back(map_data[(j * MAP_SIZE) + i]);
         }
      }
      map_pub.publish(map);
   }
   */
   return;
}

void reconfigureCb(path_planner::PathPlannerConfig & config, 
         uint32_t level) {
   goal_err             = config.goal_err;
   cone_dist            = config.cone_dist;
   max_speed            = config.max_speed;
   min_speed            = config.min_speed;
   planner_lookahead    = config.planner_lookahead;
   max_accel            = config.max_accel;
   backup_dist          = config.backup_dist;
   stuck_timeout        = config.stuck_timeout;
   cone_timeout         = config.cone_timeout;
   cone_speed           = config.cone_speed;
   track_cones          = config.track_cones;
   min_radius           = config.min_radius;
   max_radius           = config.max_radius;
}

void bumpCb(const std_msgs::Bool::ConstPtr & msg ) {
   bump = msg->data;
}

void conesCb(const visualization_msgs::Marker::ConstPtr & msg ) {
   cones = *msg;
}

void visionCb(const std_msgs::Float32::ConstPtr & msg ) {
   cone_time = ros::Time::now();
   cone = msg->data;
}

int main(int argc, char ** argv) {
   map_data = (map_type*)malloc(MAP_SIZE * MAP_SIZE * sizeof(map_type));
   // set map to empty
   for( int i=0; i<MAP_SIZE; i++ ) {
      for( int j=0; j<MAP_SIZE; j++ ) {
         map_data[i*MAP_SIZE + j] = 0;
      }
   }

   ros::init(argc, argv, "path_planner");

   ros::NodeHandle n;

   // set up tf2 transform listener
   tf2_ros::TransformListener tf2_listener(tf2_buffer);

   // subscribe to our location and current goal
   ros::Subscriber odom_sub = n.subscribe("position", 2, positionCallback);
   ros::Subscriber goal_sub = n.subscribe("current_goal", 2, goalCallback);
   ros::Subscriber laser_sub = n.subscribe("scan", 2, laserCallback);
   ros::Subscriber bump_sub = n.subscribe("bump", 2, bumpCb);
   ros::Subscriber cones_sub = n.subscribe("cone_markers", 2, conesCb);

   ros::Subscriber vision_sub = n.subscribe("top_cam/cone_angle", 2, visionCb);

   cmd_pub = n.advertise<geometry_msgs::Twist>("cmd_vel", 10);
   map_pub = n.advertise<nav_msgs::OccupancyGrid>("map", 1);
   path_pub = n.advertise<nav_msgs::Path>("path", 10);
   done_pub = n.advertise<std_msgs::Bool>("goal_reached", 1);

   dynamic_reconfigure::Server<path_planner::PathPlannerConfig> server;
   server.setCallback(boost::bind(&reconfigureCb, _1, _2));

   ROS_INFO("Path planner ready");

   ros::spin();
}
