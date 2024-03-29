#include <ros/ros.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Imu.h>

#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TransformStamped.h>

#include <Eigen/Dense>

#include <tf2/transform_datatypes.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#define _USE_MATH_DEFINES
#include <cmath>

using namespace Eigen;

nav_msgs::Odometry::ConstPtr last_odom;

ros::Publisher pose_pub, tf_pub;

constexpr int DIMENSION_ZUSTAND = 4;    // n
constexpr int DIMENSION_AKTION = 4;     // m
constexpr int DIMENSION_MESSUNG = 2;    // l

// https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles#Source_code_2
struct EulerAngles {
    double roll, pitch, yaw;
};

EulerAngles toEulerAngles(geometry_msgs::Quaternion q) {
    EulerAngles angles;

    // roll (x-axis rotation)
    double sinr_cosp = 2 * (q.w * q.x + q.y * q.z);
    double cosr_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    angles.roll = std::atan2(sinr_cosp, cosr_cosp);

    // pitch (y-axis rotation)
    double sinp = 2 * (q.w * q.y - q.z * q.x);
    if (std::abs(sinp) >= 1)
        angles.pitch = std::copysign(M_PI / 2, sinp); // use 90 degrees if out of range
    else
        angles.pitch = std::asin(sinp);

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    angles.yaw = std::atan2(siny_cosp, cosy_cosp);

    return angles;
}


void kfCallback(const sensor_msgs::Imu::ConstPtr &imu, const nav_msgs::Odometry::ConstPtr &odom) {

    if(!last_odom) {
        last_odom = odom;
        return;
    }

    geometry_msgs::PoseStamped p;
    p.header.frame_id = "world";
    p.header.stamp = imu->header.stamp;

    static Matrix<double, DIMENSION_ZUSTAND, 1> x({0,0,0,1});               // Zustand (x,y,qZ,qW)
    Matrix<double, DIMENSION_AKTION, 1> u;                                  // Aktion (dx,dy,dqZ,dqW)
    // Die Messung liefert keinen absoluten Wert für die Position. Daher wurde sich bewusst dafür entschieden, dass die Messung nur Einfluss auf die Rotation und nicht auf die zurückgelegte Distanz hat
    Matrix<double, DIMENSION_MESSUNG, 1> z;                                 // Messung (qZ,qW)

    Matrix<double, DIMENSION_ZUSTAND, DIMENSION_ZUSTAND> SigmaU;            // Kovarianzmatrix für u
    Matrix<double, DIMENSION_MESSUNG, DIMENSION_MESSUNG> SigmaZ;            // Kovarianzmatrix für z
    static Matrix<double, DIMENSION_ZUSTAND, DIMENSION_ZUSTAND> SigmaT;     // Kovarianzmatrix für μ bzw. x
    Matrix<double, DIMENSION_ZUSTAND, DIMENSION_ZUSTAND> E;                 // Einheitsmatrix
    E.setIdentity();

    Matrix<double, DIMENSION_ZUSTAND, DIMENSION_ZUSTAND> A;
    A.setIdentity();

    Matrix<double, DIMENSION_ZUSTAND, DIMENSION_AKTION> B;
    B.setIdentity();

    // Konvertiert die Zustandsmatrix in den Raum der Messung
    Matrix<double, DIMENSION_MESSUNG, DIMENSION_ZUSTAND> H;
    H << 0,0,1,0,
        0,0,0,1;

    geometry_msgs::Quaternion tmpQ;
    tmpQ.z = x(2);
    tmpQ.w = x(3);

    EulerAngles x_eu = toEulerAngles(tmpQ);

    double odom_delta_t = last_odom->header.stamp.toSec() - odom->header.stamp.toSec();

    // Relative Bewegung zu er letzten bestimmten Position wird mithilfe der Odometrie bestimmt
    u(0) = -std::cos(x_eu.yaw) * odom->twist.twist.linear.x * odom_delta_t;
    u(1) = -std::sin(x_eu.yaw) * odom->twist.twist.linear.x * odom_delta_t;
    u(2) = last_odom->pose.pose.orientation.z - odom->pose.pose.orientation.z;
    u(3) = last_odom->pose.pose.orientation.w - odom->pose.pose.orientation.w;

    z(0) = imu->orientation.z;
    z(1) = imu->orientation.w;

    for(int i = 0; i < DIMENSION_ZUSTAND; i++) {
        SigmaT(i,i) = 1;
    }

    // Kovarianz aus den Sensordaten wird in die Matrizen geschrieben
    SigmaU(0,0) = odom->pose.covariance.at(0);
    SigmaU(1,1) = odom->pose.covariance.at(6+1);
    SigmaU(2,2) = odom->pose.covariance.at(35);
    SigmaU(3,3) = odom->pose.covariance.at(35);

    SigmaZ(0,0) = imu->orientation_covariance.at(8);
    SigmaZ(1,1) = imu->orientation_covariance.at(8);

    // Prädiktion
    x = A*x + B*u;
    SigmaT = A*SigmaT*A.transpose() + SigmaU;

    // Korrektur
    auto K = SigmaT*H.transpose() * (H*SigmaT*H.transpose() + SigmaZ).inverse();
    x = x + K*(z - H*x);
    SigmaT = (E - K*H) * SigmaT;
    //

    last_odom = odom;

    p.pose.position.x = x(0);
    p.pose.position.y = x(1);
    p.pose.orientation.z = x(2);
    p.pose.orientation.w = x(3);

    // Wandelt die Pose in eine Transformation um und published diese. Wir verstehen allerdings nicht ganz, was hier der gewünschte Effekt ist.
    geometry_msgs::Transform tfp;
    tfp.rotation = p.pose.orientation;
    tfp.translation.x = p.pose.position.x;
    tfp.translation.y = p.pose.position.y;
    tfp.translation.z = p.pose.position.z;

    pose_pub.publish(p);
    tf_pub.publish(tfp);
}

int main(int argc, char **argv) {

    ros::init(argc, argv, "kf_odom_imu_node");

    ros::NodeHandle nh;

    // publisher
    pose_pub = nh.advertise<geometry_msgs::PoseStamped>("/kfPose", 1000);
    tf_pub = nh.advertise<geometry_msgs::Transform>("/kfTransform", 1000);


    // subscriber
    message_filters::Subscriber<sensor_msgs::Imu> imu_sub(nh, "/imu/data", 1);
    message_filters::Subscriber<nav_msgs::Odometry> odom_sub(nh, "/odom", 1);

    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Imu, nav_msgs::Odometry> MySyncPolicy;
    message_filters::Synchronizer<MySyncPolicy> sync(MySyncPolicy(10), imu_sub, odom_sub);
    sync.registerCallback(boost::bind(&kfCallback, _1, _2));

    ros::spin();

    return 0;
}
