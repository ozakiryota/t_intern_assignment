#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_listener.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/common/centroid.h>
#include <pcl/common/transforms.h>
#include <pcl/visualization/cloud_viewer.h>

class VehicleDetection{
	private:
		/*node handle*/
		ros::NodeHandle nh;
		ros::NodeHandle nhPrivate;
		/*subscribe*/
		ros::Subscriber sub_pc;
		tf::TransformListener tf_listener;
		/*pcl objects*/
		pcl::visualization::PCLVisualizer viewer {"vehicle_detection"};
		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud {new pcl::PointCloud<pcl::PointXYZ>};
		std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> clusters;
		pcl::PointCloud<pcl::PointXYZ>::Ptr centroids {new pcl::PointCloud<pcl::PointXYZ>};
		pcl::PointCloud<pcl::PointXYZ>::Ptr centroids_last {new pcl::PointCloud<pcl::PointXYZ>};
		/*objects*/
		Eigen::Vector3d self_velocity;
		std::vector<Eigen::Vector3d> vehicle_velocities;
		/*parameters*/
		std::string parent_frame_name;
		double cluster_tolerance;
		int min_cluster_size;
		double association_distance;
	public:
		VehicleDetection();
		void CallbackPC(const sensor_msgs::PointCloud2ConstPtr &msg);
		void Transformation(void);
		void Clustering(void);
		void Association(void);
		void Visualization(void);
		double MsToKmh(double ms);
};

VehicleDetection::VehicleDetection()
	:nhPrivate("~")
{
	sub_pc = nh.subscribe("/cloud", 1, &VehicleDetection::CallbackPC, this);
	viewer.setBackgroundColor(1, 1, 1);
	viewer.addCoordinateSystem(3.0, "axis");
	viewer.setCameraPosition(0.0, 0.0, 180.0, 0.0, 0.0, 0.0);

	nhPrivate.param("parent_frame_name", parent_frame_name, std::string("/odom"));
	std::cout << "parent_frame_name = " << parent_frame_name << std::endl;
	nhPrivate.param("cluster_tolerance", cluster_tolerance, 0.1);
	std::cout << "cluster_tolerance = " << cluster_tolerance << std::endl;
	nhPrivate.param("min_cluster_size", min_cluster_size, 100);
	std::cout << "min_cluster_size = " << min_cluster_size << std::endl;
	nhPrivate.param("association_distance", association_distance, 1.0);
	std::cout << "association_distance = " << association_distance << std::endl;
}

void VehicleDetection::CallbackPC(const sensor_msgs::PointCloud2ConstPtr &msg)
{
	/* std::cout << "CALLBACK PC" << std::endl; */

	pcl::fromROSMsg(*msg, *cloud);
	std::cout << "==========" << std::endl;
	std::cout << "cloud->points.size() = " << cloud->points.size() << std::endl;

	/*initialize*/
	clusters.clear();
	centroids->points.clear();
	centroids->header = cloud->header;

	/*execute*/
	Clustering();
	Transformation();
	Association();
	Visualization();

	/*buffer*/
	*centroids_last = *centroids;
}

void VehicleDetection::Transformation(void)
{
	/*current*/
	tf::StampedTransform tf_trans_current;
	ros::Time time_current;
	pcl_conversions::fromPCL(centroids->header.stamp, time_current);
	try{
		tf_listener.lookupTransform(
			parent_frame_name,
			centroids->header.frame_id,
			time_current,
			tf_trans_current
		);
	}
	catch(tf::TransformException ex){
		std::cout << "Error: current tf listen" << std::endl;
		ROS_ERROR("%s", ex.what());
		ros::Duration(1.0).sleep();
	}
	/*last*/
	tf::StampedTransform tf_trans_last;
	ros::Time time_last;
	pcl_conversions::fromPCL(centroids_last->header.stamp, time_last);
	try{
		tf_listener.lookupTransform(
			parent_frame_name,
			centroids_last->header.frame_id,
			time_last,
			tf_trans_last
		);
	}
	catch(tf::TransformException ex){
		std::cout << "Error: last tf listen" << std::endl;
		ROS_ERROR("%s", ex.what());
		ros::Duration(1.0).sleep();
	}
	/*transformation-pc*/
	tf::Quaternion relative_rotation = tf_trans_last.getRotation()*tf_trans_current.getRotation().inverse();
	relative_rotation.normalize();	
	Eigen::Quaternionf rotation(relative_rotation.w(), relative_rotation.x(), relative_rotation.y(), relative_rotation.z());
	tf::Quaternion q_global_move_pc(
		tf_trans_last.getOrigin().x() - tf_trans_current.getOrigin().x(),
		tf_trans_last.getOrigin().y() - tf_trans_current.getOrigin().y(),
		tf_trans_last.getOrigin().z() - tf_trans_current.getOrigin().z(),
		0.0
	);
	tf::Quaternion q_local_move_pc = tf_trans_last.getRotation().inverse()*q_global_move_pc*tf_trans_last.getRotation();
	Eigen::Vector3f offset(q_local_move_pc.x(), q_local_move_pc.y(), q_local_move_pc.z());
	pcl::transformPointCloud(*centroids_last, *centroids_last, offset, rotation);
	/*transformation-self*/
	tf::Quaternion q_global_move_self(
		tf_trans_current.getOrigin().x() - tf_trans_last.getOrigin().x(),
		tf_trans_current.getOrigin().y() - tf_trans_last.getOrigin().y(),
		tf_trans_current.getOrigin().z() - tf_trans_last.getOrigin().z(),
		0.0
	);
	tf::Quaternion q_local_move_self = tf_trans_current.getRotation().inverse()*q_global_move_self*tf_trans_current.getRotation();
	/*velocities*/
	double dt = (time_current - time_last).toSec();
	self_velocity = {
		q_local_move_self.x()/dt,
		q_local_move_self.y()/dt,
		q_local_move_self.z()/dt
	};
	std::cout << "self_velocity.norm() = " << self_velocity.norm() << std::endl;
}

void VehicleDetection::Clustering(void)
{
	double time_start = ros::Time::now().toSec();

	/*clustering*/
	pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ>);
	tree->setInputCloud(cloud);
	std::vector<pcl::PointIndices> cluster_indices;
	pcl::EuclideanClusterExtraction<pcl::PointXYZ> ece;
	ece.setClusterTolerance(cluster_tolerance);
	ece.setMinClusterSize(min_cluster_size);
	ece.setMaxClusterSize(cloud->points.size());
	ece.setSearchMethod(tree);
	ece.setInputCloud(cloud);
	ece.extract(cluster_indices);

	std::cout << "cluster_indices.size() = " << cluster_indices.size() << std::endl;

	/*dividing*/
	pcl::ExtractIndices<pcl::PointXYZ> ei;
	ei.setInputCloud(cloud);
	ei.setNegative(false);
	for(size_t i=0;i<cluster_indices.size();i++){
		/*extract*/
		pcl::PointCloud<pcl::PointXYZ>::Ptr tmp_clustered_points (new pcl::PointCloud<pcl::PointXYZ>);
		pcl::PointIndices::Ptr tmp_clustered_indices (new pcl::PointIndices);
		*tmp_clustered_indices = cluster_indices[i];
		ei.setIndices(tmp_clustered_indices);
		ei.filter(*tmp_clustered_points);
		/*input*/
		clusters.push_back(tmp_clustered_points);

		/*centroid*/
		Eigen::Vector4f tmp_centroid4f;
		pcl::compute3DCentroid(*cloud, cluster_indices[i], tmp_centroid4f);
		pcl::PointXYZ tmp_p;
		tmp_p.x = tmp_centroid4f[0];
		tmp_p.y = tmp_centroid4f[1];
		tmp_p.z = tmp_centroid4f[2];
		centroids->points.push_back(tmp_p);
	}

	std::cout << "clustering time [s] = " << ros::Time::now().toSec() - time_start << std::endl;
}

void VehicleDetection::Association(void)
{
	std::cout << "Association" << std::endl;
	/*initialize*/
	pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
	std::vector<int> list_index;
	std::vector<float> list_squared_distance;
	vehicle_velocities.resize(centroids->points.size());
	/*search*/
	if(!centroids_last->points.empty()){
		kdtree.setInputCloud(centroids_last);
		for(size_t i=0;i<centroids->points.size();++i){
			kdtree.radiusSearch(centroids->points[i], association_distance, list_index, list_squared_distance);
			if(!list_index.empty()){
				ros::Time dt_ros;
				pcl_conversions::fromPCL(centroids->header.stamp - centroids_last->header.stamp, dt_ros);
				double dt = dt_ros.toSec();
				vehicle_velocities[i] = {
					(centroids->points[i].x - centroids_last->points[list_index[0]].x)/dt,
					(centroids->points[i].y - centroids_last->points[list_index[0]].y)/dt,
					(centroids->points[i].z - centroids_last->points[list_index[0]].z)/dt
				};
				/*relative to absolute*/
				// vehicle_velocities[i] += self_velocity;
			}
			else{
				vehicle_velocities[i] = {
					NAN,
					NAN,
					NAN
				};
			}
		}
	}
	std::cout << "Association done" << std::endl;
}

void VehicleDetection::Visualization(void)
{
	std::cout << "Visualization" << std::endl;
	/*initialize*/
	viewer.removeAllPointClouds();
	viewer.removeAllShapes();

	/*cloud*/
	viewer.addPointCloud(cloud, "cloud");
	viewer.setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_COLOR, 0.0, 0.0, 0.0, "cloud");
	viewer.setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 3, "cloud");
	/*clusters*/
	double rgb[3] = {};
	const int channel = 3;
	const double step = ceil(pow(clusters.size()+2, 1.0/(double)channel));	//exept (000),(111)
	const double max = 1.0;
	for(size_t i=0;i<clusters.size();i++){
		std::string name = "cluster_" + std::to_string(i);
		rgb[0] += 1/step;
		for(int j=0;j<channel-1;j++){
			if(rgb[j]>max){
				rgb[j] -= max + 1/step;
				rgb[j+1] += 1/step;
			}
		}
		viewer.addPointCloud(clusters[i], name);
		viewer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR, rgb[0], rgb[1], rgb[2], name);
		viewer.setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 5, name);

		/*centroids*/
		if(!std::isnan(vehicle_velocities[i](0)) && !std::isnan(vehicle_velocities[i](1)) && !std::isnan(vehicle_velocities[i](2))){
			/*text*/
			const int digits = 2;
			std::vector<std::ostringstream> oss(3);
			for(int j=0;j<vehicle_velocities[i].size();++j){
				double kmh = MsToKmh(vehicle_velocities[i](j));
				oss[j] << std::fixed << std::setprecision(digits) << kmh;
			}
			std::string text = "(" 
				+ oss[0].str() + ", "
				+ oss[1].str() + ", "
				+ oss[2].str() +
			")[km/h]";
			std::string text_id = "text_" + std::to_string(i);
			viewer.addText3D(text, centroids->points[i], 1.0, 0.0, 0.0, 0.0, text_id);
			/*arrow*/
			std::string arrow_id = "arrow_" + std::to_string(i);
			ros::Time dt_ros;
			pcl_conversions::fromPCL(centroids->header.stamp - centroids_last->header.stamp, dt_ros);
			double dt = dt_ros.toSec();
			pcl::PointXYZ tmp_end;
			tmp_end.x = centroids->points[i].x + vehicle_velocities[i](0)*dt;
			tmp_end.y = centroids->points[i].y + vehicle_velocities[i](1)*dt;
			tmp_end.z = centroids->points[i].z + vehicle_velocities[i](2)*dt;
			viewer.addArrow(tmp_end, centroids->points[i], 1.0, 0.0, 0.0, false, arrow_id);
		}
	}
	
	viewer.spinOnce();
	std::cout << "Visualization done" << std::endl;
}

double VehicleDetection::MsToKmh(double ms)
{
	return ms*3600*0.001;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "vehicle_detection");
	
	VehicleDetection vehicle_detection;

	ros::spin();
}
