/* cone_detector
 * 
 * detect cones in various types of sensor data
 *
 * Based on a paper by Joao Xavier, Marco Pacheco, Daniel Castro, Antonio Ruano
 *  and Urbano Nunes
 *
 * Author: Austin Hendrix
 *
 * Sleep: none
 */

#include <ros/ros.h>

#include <list>
#include <boost/foreach.hpp>

#include <geometry_msgs/PointStamped.h>
#include <sensor_msgs/LaserScan.h>
#include <tf/transform_listener.h>
#include <visualization_msgs/Marker.h>

#include <dynamic_reconfigure/server.h>
#include <cone_detector/ConeDetectorConfig.h>

double dist(geometry_msgs::Point a, geometry_msgs::Point b) {
   return hypot(a.x - b.x, a.y - b.y);
}

double dist(geometry_msgs::PointStamped a, geometry_msgs::PointStamped b) {
   return dist(a.point, b.point);
}

class ConeDetector {
private:
   ros::NodeHandle n;
   tf::TransformListener listener;
   ros::Subscriber laser_sub;
   ros::Publisher marker_pub;
   dynamic_reconfigure::Server<cone_detector::ConeDetectorConfig> server;

   // last seen and point
   typedef std::pair<ros::Time, geometry_msgs::Point> cone_type;
   typedef std::list<cone_type> cone_list;
   cone_list cones;

   double grouping_threshold;
   int min_circle_size;
   double std_dev_threshold;
   double same_cone_threshold;
   double min_cone_radius;
   double max_cone_radius;
public:
   ConeDetector() : listener(n, ros::Duration(20.0)) {
      laser_sub = n.subscribe("scan", 1, &ConeDetector::laserCallback, this);
      marker_pub = n.advertise<visualization_msgs::Marker>("cone_markers", 1);;
      
      min_circle_size = 4;
      grouping_threshold = 0.05;
      std_dev_threshold = 15.0;
      same_cone_threshold = 0.25;
      min_cone_radius = 0.1;
      max_cone_radius = 0.2;

      server.setCallback(boost::bind(&ConeDetector::reconfigureCb, 
               this, _1, _2));
   }

   void laserCallback(const sensor_msgs::LaserScan::ConstPtr & msg) {
      std::list<std::list<geometry_msgs::Point> > groups;
      groups.push_back(std::list<geometry_msgs::Point>());

      // range segmentation
      geometry_msgs::PointStamped prev;
      geometry_msgs::PointStamped a, b;

      a.header = msg->header;
      //a.header.stamp = ros::Time(0);
      a.point.z = 0;

      listener.waitForTransform("/odom", msg->header.frame_id,
            msg->header.stamp, ros::Duration(0.5));

      try {
         double theta = msg->angle_min;
         for( size_t i=0; i < msg->ranges.size(); ++i, 
               theta += msg->angle_increment) {
            double r = msg->ranges[i];
            if( r >= msg->range_min ) {
               a.point.x = r * cos(theta);
               a.point.y = r * sin(theta);
               listener.transformPoint("/odom", a, b);

               double d = dist(b, prev);
               if( d > grouping_threshold ) {
                  groups.push_back(std::list<geometry_msgs::Point>());
               }
               groups.back().push_back(b.point);
               prev = b;
            }
         }
      } catch(tf::TransformException e) {
         ROS_ERROR("%s", e.what());
      }

      // new cones
      cone_list new_cones;

      // set up markers
      visualization_msgs::Marker markers;
      markers.header.frame_id = "/odom";
      markers.header.stamp = msg->header.stamp;
      markers.type = visualization_msgs::Marker::POINTS;
      markers.action = visualization_msgs::Marker::MODIFY;

      markers.color.r = 1.0;
      markers.color.g = 0.0;
      markers.color.b = 0.0;
      markers.color.a = 1.0;

      markers.scale.x = 0.05;
      markers.scale.y = 0.05;
      markers.scale.z = 0.05;

      // circle detection
      BOOST_FOREACH(std::list<geometry_msgs::Point> group, groups) {
         if( group.size() > min_circle_size ) {
            // calculate inscribed angles for each inner point in group
            std::list<double> angles;
            double avg_angle = 0;
            geometry_msgs::Point first = group.front();
            geometry_msgs::Point last = group.back();
            geometry_msgs::Point center;
            int i=0;
            int j=0;
            BOOST_FOREACH(geometry_msgs::Point p, group) {
               if( i > 0 && i < (group.size() - 1) ) {
                  // compute inscribed angle
                  double angle = atan2(first.y - p.y, first.x - p.x) - 
                     atan2(last.y - p.y, last.x - p.x);
                  angles.push_back(angle);
                  avg_angle += angle;
                  ++j;
               }
               if( i == group.size()/2 ) {
                  center = p;
               }
               ++i;
            }
            // check prerequisite for circle
            bool circle = true;
            {
               double theta = atan2(last.x - first.x, last.y - first.y);
               double x2 = - (((center.x-first.x) * cos(theta)) - 
                     ((center.y-first.y) * sin(theta)));
               if( 0.1 * dist(first, last) > x2 ) circle = false;
               if( 0.7 * dist(first, last) < x2 ) circle = false;
            }

            if( circle ) {
               // average inscribed angle
               avg_angle /= (group.size() - 2);

               // standard deviation
               double std_dev = 0;
               BOOST_FOREACH(double a, angles) {
                  std_dev += (a - avg_angle) * (a - avg_angle);
               }
               std_dev /= (group.size() - 2);
               std_dev = sqrt(std_dev) * 180.0 / M_PI;
               if( std_dev < std_dev_threshold ) {
                  // compute center of circle
                  double theta = atan2(last.y - first.y, last.x - first.x);
                  double d = dist(first, last);

                  double x = d / 2;
                  double y = d * tan(avg_angle - M_PI/2.0);

                  center.x = first.x + x * cos(theta) - y*sin(theta);
                  center.y = first.y + y * cos(theta) + x*sin(theta);

                  double r = hypot(x, y);

                  if( r > min_cone_radius && r < max_cone_radius ) {
                     //markers.points.push_back(center);

                     ROS_INFO("Found circle with radius %lf", r);

                     if( cones.size() > 0 ) {
                        cone_list::iterator nearest = cones.begin();
                        d = dist(cones.front().second, center);
                        // determine if this is a cone we've seen before
                        for( cone_list::iterator itr = cones.begin(); 
                              itr != cones.end(); ++itr ) {
                           if( dist(itr->second, center) < d ) {
                              d = dist(itr->second, center);
                              nearest = itr;
                           }
                        }
                        if( dist(nearest->second, center) < 
                              same_cone_threshold ) {
                           cones.erase(nearest);
                        }
                     }
                     new_cones.push_back(cone_type(ros::Time::now(), center));
                     // markers at ends of arc
                     //markers.points.push_back(first);
                     //markers.points.push_back(last);
                  }
               }
            }
         }
      }

      BOOST_FOREACH(cone_type p, cones) {
         // TODO: dynamic_reconfigure parameter
         if( p.first > (ros::Time::now() - ros::Duration(2.0)) ) {
            new_cones.push_back(p);
         }
      }
      cones.clear();
      BOOST_FOREACH(cone_type p, new_cones) {
         markers.points.push_back(p.second);
         cones.push_back(p);
      }
      marker_pub.publish(markers);
   }

   void reconfigureCb(cone_detector::ConeDetectorConfig & config, 
         uint32_t level) {
      grouping_threshold  = config.grouping_threshold;
      min_circle_size     = config.min_circle_size;
      std_dev_threshold   = config.std_dev_threshold;
      same_cone_threshold = config.same_cone_threshold;
      min_cone_radius     = config.min_cone_radius;
      max_cone_radius     = config.max_cone_radius;
   }
};

int main(int argc, char ** argv) {
   ros::init(argc, argv, "cone_detector");

   ConeDetector detector;

   ros::spin();
}
