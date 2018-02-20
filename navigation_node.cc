//
//
//
//
//

#include "navigation_node.h"

#include <iostream>
#include <stdlib.h>     /* srand, rand */
#include <time.h>       /* time */
#include <math.h>

#include <map>
#include <vector>
#include <queue>
#include <string>
#include <set>

#include "Eigen/Core"
#include "Eigen/Geometry"
#include "cairo/cairo.h"
#include "cartographer/common/mutex.h"
#include "cartographer/common/port.h"
#include "cartographer/io/image.h"
#include "cartographer/io/submap_painter.h"
#include "cartographer/mapping/id.h"
#include "cartographer/transform/rigid_transform.h"
#include "cartographer_ros/msg_conversion.h"
#include "cartographer_ros/node_constants.h"
#include "cartographer_ros/submap.h"
#include "cartographer_ros_msgs/SubmapList.h"
#include "cartographer_ros_msgs/SubmapQuery.h"
#include "utils.h"
#include "nav_msgs/Path.h"
#include "ros/ros.h"


namespace cartographer_ros{
namespace cartographer_ros_navigation {
namespace{
    
using ::cartographer::io::PaintSubmapSlicesResult;
using ::cartographer::io::SubmapSlice;
using ::cartographer::mapping::SubmapId;

const char kSubmapListTopicName [] = "/submap_list";
const char kSubmapQueryServiceName [] = "/submap_query";
const int kMaxNeighborNum = 3;
const int kFinishVersion = 180;
const int kProbabilityGridWidth = 100;
const int kProbabilityGridHeight = 100;
const int kOccupyThreshhold = 64;
const int kMaxRRTNodeNum = 100;
const int kStepToCheckReachEndPoint = 5;
const float kOccupyGridResolution = 0.05;
const float kDistance2ThresholdForAdding = 5.0;
const float kDistance2ThresholdForUpdating = 1.0;
const float kRotationThresholdForUpdating = 1.0;
const float kProbabilityGridResolution = 0.05;
const float kProbabilityOfChooseEndPoint = 0.2;
    
class ComparePair{
public:
    bool operator ()(std::pair<SubmapIndex,float>& a, std::pair<SubmapIndex,float>& b){
        return a.second > b.second;
    }
};

inline bool IsValueFree(int val){
    return val>=0 && val<kOccupyThreshhold;
}

} // namespace
    
// constructor
NavigationNode::NavigationNode(){

    cartographer::common::MutexLocker lock(&mutex_);
    submap_list_subscriber_ = node_handle_.subscribe<cartographer_ros_msgs::SubmapList>(kSubmapListTopicName,10, &NavigationNode::UpdateRoadMap,this);
    submap_query_client_ = node_handle_.serviceClient<cartographer_ros_msgs::SubmapQuery>(kSubmapQueryServiceName);

    // For test
    clicked_point_subscriber_ = node_handle_.subscribe<geometry_msgs::PointStamped>("/clicked_point",1,&NavigationNode::NavigateToClickedPoint, this);
    path_publisher_ = node_handle_.advertise<::nav_msgs::Path>("/test_path", 10);
    path_publisher_timer_ = node_handle_.createWallTimer(::ros::WallDuration(0.1), &NavigationNode::PublishPath, this);

    std::cout<<"Successfully Create NavigationNode"<<std::endl;
}

// Return the cloest SubmapID if pose is free in this submap
SubmapIndex NavigationNode::CloestSubmap(const geometry_msgs::Point& point) {
    std::cout<<"Try to find cloest_submap"<<std::endl;
    float min_distance = FLT_MAX;
    SubmapIndex cloest_submap = -1;
    for(auto& submap:submap_){
        int val = IsLocalFree(point, submap.first);
        if(!IsValueFree(val)) continue;
        float distance2 = Distance2BetweenPoint(submap.second.pose.position,point);
        if(distance2<min_distance){
            min_distance = distance2;
            cloest_submap = submap.first;
        }
    }
    return cloest_submap;
}

// Add new submap grid to submap_grad_ using SubmapQuery
void NavigationNode::AddSubmapGrid(SubmapIndex submap_index){
    std::cout<<"Begin to add submap "<<submap_index<<" into submap_grid_"<<std::endl;
    // clear the existing submap
    submap_grid_.erase(submap_index);
    
    // first fetch the submaptexture
    cartographer_ros_msgs::SubmapEntry& submap_entry = submap_[submap_index];
    const SubmapId id{submap_entry.trajectory_id, submap_entry.submap_index};
    
    auto fetched_textures = ::cartographer_ros::FetchSubmapTextures(id, &submap_query_client_);
    const auto submap_texture = fetched_textures->textures.begin();
    
    // use fake map to do it (map only contain one element)
    std::map<SubmapId, SubmapSlice> fake_submap_slices;
    ::cartographer::io::SubmapSlice& submap_slice = fake_submap_slices[id];
    
    submap_slice.pose = ToRigid3d(submap_entry.pose);
    submap_slice.metadata_version = submap_entry.submap_version;
    
    // push fetched texture to slice and draw the texture
    submap_slice.version = fetched_textures->version;
    submap_slice.width = submap_texture->width;
    submap_slice.height = submap_texture->height;
    submap_slice.slice_pose = submap_texture->slice_pose;
    submap_slice.resolution = submap_texture->resolution;
    submap_slice.cairo_data.clear();
    submap_slice.surface = ::cartographer::io::DrawTexture(submap_texture->pixels.intensity,
                                                           submap_texture->pixels.alpha,
                                                           submap_texture->width, submap_texture->height,
                                                           &submap_slice.cairo_data);
    
    
    // Paint the texture
    auto painted_slices = cartographer::io::PaintSubmapSlices(fake_submap_slices, kOccupyGridResolution);
    
    // Convert painted surface into occupied grid
    auto& submap_grid = submap_grid_[submap_index];
    const Eigen::Array2f& origin = painted_slices.origin;
    cairo_surface_t* surface = painted_slices.surface.get();
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    
    submap_grid.submap_index = submap_index;
    submap_grid.resolution = kOccupyGridResolution;
    submap_grid.width = width;
    submap_grid.height = height;
    submap_grid.x0 = - origin.x() * kOccupyGridResolution;
    submap_grid.y0 =  (origin.y() - height) * kOccupyGridResolution;
    
    const uint32_t* pixel_data = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(surface));
    submap_grid.data.reserve(width*height);
    for(int y=0;y<height;y++){
        for(int x=0;x<width;x++){
            const uint32_t packed = pixel_data[(height-y-1)*width+x];
            const unsigned char color = packed >> 16;
            const unsigned char observed = packed >> 8;
            const int value =
            observed == 0
            ? -1
            : ::cartographer::common::RoundToInt((1. - color / 255.) * 100.);
            submap_grid.data.push_back(value);
        }
    }
    std::cout<<"Succeed to add submap "<<submap_index<<" into submap_grid_"<<std::endl;
}
    
// Return whether a point is free in a submap
int NavigationNode::IsLocalFree(const geometry_msgs::Point& point,   // only use x & y in 2D case
                                SubmapIndex submap_index) const {
    const auto& submap_grid = submap_grid_[submap_index];
    if(submap_grid!=submap_grid_.end()){
        const int x = (point.x - submap_grid.x0) / submap_grid.resolution;
        const int y = (point.y - submap_grid.y0) / submap_grid.resolution;
        if(x>=0 && x<submap_grid.width && y>=0 && y<submap_grid.height){
            return submap_grid.data.at(y*submap_grid.width+x);
        } else{
            std::cout<<"Point is out of submap range"<<std::endl;
            return -1;
        }
    } else{
        std::cout<<"Submap "<<submap_index<<" do not exist!"<<std::endl;
        return -1;
    }
}
    

// TODO: Return a free path from starting position to end postion using RRT
Path NavigationNode::PlanPathRRT(const geometry_msgs::Point& start_point,
                                 const geometry_msgs::Point& end_point) {
    std::cout<<"Begin planning!"<<std::endl;
    Path path;
    SubmapIndex start_submap_index = CloestSubmap(start_point);
    SubmapIndex end_submap_index = CloestSubmap(end_point);
    std::cout<<"start_submap_index: "<<start_submap_index<<", "<<"end_submap_index: "<<end_submap_index<<std::endl;
    // check
    if(start_submap_index<0 || end_submap_index<0){
        std::cout<<"goal is not free!"<<std::endl;
        return {};
    }
    if(start_submap_index==end_submap_index){
        std::vector<SubmapIndex> submap_indexes = {start_submap_index};
        path = LocalPlanPathRRT(start_point,end_point,submap_indexes);
        AddDisplayPath(path);
        return path;
    }
    // connecting start to start_submap
    std::cout<<"connecting start to start_submap"<<std::endl;
    auto startpath = LocalPlanPathRRT(start_point,submap_[start_submap_index].pose.position,
                                    std::vector<SubmapIndex> ({start_submap_index}));
    if(!startpath.empty()){
        path.insert(path.end(),startpath.begin(),startpath.end());
    } else{
        std::cout<<"fail to connect start to start_submap"<<std::endl;
        return {};
    }
    // connecting start_submap to end_submap (graph search)
    std::cout<<"connecting start_submap to end_submap (graph search)"<<std::endl;
    auto midpath = ConnectingSubmap(start_submap_index,end_submap_index);
    if(!midpath.empty()){
        path.insert(path.end(),midpath.begin(),midpath.end());
    } else{
        std::cout<<"Fail to connecting start_submap to end_submap (graph search)"<<std::endl;
        return {};
    }
    
    // connecting end_submap to end point
    std::cout<<" connecting end to end_submap"<<std::endl;
    auto endpath = LocalPlanPathRRT(submap_[end_submap_index].pose.position,end_point,
                                      std::vector<SubmapIndex> ({end_submap_index}));
    if(!endpath.empty()){
        path.insert(path.end(),endpath.begin(),endpath.end());
    } else{
        std::cout<<" fail to connecting end to end_submap"<<std::endl;
        return {};
    }
    std::cout<<path<<std::endl;
    AddDisplayPath(path);
    return path;
    
}


Path NavigationNode::ConnectingSubmap(SubmapIndex start_idx, SubmapIndex end_idx){
    const auto& start_point = submap_[start_idx].pose.position;
    std::priority_queue<std::pair<SubmapIndex,float>,
                        std::vector<std::pair<SubmapIndex,float>>,
                        ComparePair> submap_to_visit;
    for(auto& submap:submap_){
        float distance2 = Distance2BetweenPoint(start_point,submap.second.pose.position);
        submap_to_visit.emplace(submap.first,distance2);
    }
    
    std::vector<float> visited_submap_distance (submap_.size(),FLT_MAX);
    visited_submap_distance[start_idx] = 0.0;
    std::vector<SubmapIndex> previous_submap (submap_.size(),-1);
    bool find_end_idx = false;
    while(!submap_to_visit.empty()&&!find_end_idx){
        auto& current_submap = submap_to_visit.top();
        std::cout<<"Visiting "<<current_submap.first<<std::endl;
        auto& current_connections = road_map_[current_submap.first];
        for(const auto& entry:current_connections){
            if(entry.second.distance + visited_submap_distance[current_submap.first] <
               visited_submap_distance[entry.first]){
                visited_submap_distance[entry.first] = entry.second.distance + visited_submap_distance[current_submap.first];
                previous_submap[entry.first] = current_submap.first;
                if(entry.first==end_idx) {find_end_idx = true;break;}
            }
        }
        submap_to_visit.pop();
    }
    if(previous_submap[end_idx]==-1) return {};
    Path path;
    SubmapIndex idx = end_idx;
    while(idx!=-1){
        SubmapIndex previous_idx = previous_submap[idx];
        if(previous_idx!=-1){
            auto connection = road_map_[previous_idx].find(idx);
            if(connection==road_map_[previous_idx].end()){
                std::cout<<"Error! There's no road map between "<<previous_idx<<" and "<<idx<<std::endl;
                return {};
            }
            path.insert(path.begin(),connection->second.path.begin(),connection->second.path.end());
        } 
        idx = previous_submap[idx];
    }
    return path;
}
    
// Return a free point in submap
geometry_msgs::Point NavigationNode::RandomFreePoint(const std::vector<SubmapIndex>& submap_indexes){
    // init rand seed
    std::cout<<"Begin Generate Random Point"<<std::endl;
    srand (time(NULL));
    while(true){
        int random_idx = rand() % submap_indexes.size();
        auto& submap_grid = submap_grid_.find(submap_indexes[random_idx])->second;
        int random_x = rand() % submap_grid.width;
        int random_y = rand() % submap_grid.height;
        int val = submap_grid.data[random_y * submap_grid.width + random_x];
        if(val>=0 && val<kOccupyThreshhold){
            std::cout<<"Try to Generate one point"<<std::endl;
            geometry_msgs::Point point;
            point.x = random_x * submap_grid.resolution + submap_grid.x0;
            point.y = random_y * submap_grid.resolution + submap_grid.y0;
            point.z = 0.0;
            return point;
        }
    }
}
    
// Return the pointer to the nearest node to target in RRT
RRTreeNode* NavigationNode::NearestRRTreeNode(RRTreeNode* root, const geometry_msgs::Point& target){
    std::cout<<"Try to find the nearest node in RRT"<<std::endl;
    float min_distance = FLT_MAX;
    RRTreeNode* nearest_node = root;
    std::queue<RRTreeNode*> node_to_visit;
    node_to_visit.push(root);
    while(!node_to_visit.empty()){
        RRTreeNode* current_node = node_to_visit.front();
        //std::cout<<current_node->point<<": parent"<<(current_node->parent_node!=nullptr)<<" ,children: "<<current_node->children_node.size()<<std::endl;
        float distance2 = Distance2BetweenPoint(target,current_node->point);
        if(distance2 < min_distance){
            min_distance = distance2;
            nearest_node = current_node;
        }
        //std::cout<<distance2<<std::endl;
        for(RRTreeNode* node:current_node->children_node ){
            if( current_node->children_node.size()>20) break;
            node_to_visit.push(node);
        }
        //std::cout<<"Push"<<std::endl;
        if(!node_to_visit.empty()) node_to_visit.pop();
        //std::cout<<"PoP"<<std::endl;
    }
    std::cout<<"End to find the nearest node in RRT"<<std::endl;
    return nearest_node;
}

// destropy the RRT tree
void NavigationNode::DestroyRRTree(RRTreeNode* root){
    if(root!=nullptr){
        for(auto& child:root->children_node){
            DestroyRRTree(child);
        }
        delete(root);
    }
}
//
Path NavigationNode::LocalPlanPathRRT(const geometry_msgs::Point& start_point,
                                      const geometry_msgs::Point& end_point,
                                      const std::vector<SubmapIndex> submap_indexes){
    // Naively check the straight line between two points
    if(IsPathLocalFree(start_point,end_point,submap_indexes)){
        std::cout<<"Directly connect two submaps: "<<start_point<<","<<end_point<<std::endl;
        return {start_point,end_point};
    }
    Path path;
    srand(time(NULL));
    RRTreeNode* root = new RRTreeNode (start_point);
    for(int node_num=0;node_num<kMaxRRTNodeNum;node_num++){
        geometry_msgs::Point next_point;
        if((rand() % 1000) / 1000.0 < kProbabilityOfChooseEndPoint){
            // choose end point
            next_point = (end_point);
        } else{
            // random generate point
            next_point =  RandomFreePoint(submap_indexes);
        }
        // search the nearest tree node (better method expeced but now just linear search

        RRTreeNode* nearest_node = NearestRRTreeNode(root,next_point);
        if(!IsPathLocalFree(nearest_node->point,next_point,submap_indexes)) continue;
        //std::cout<<"next_point: "<<next_point<<"node_num: "<<node_num<<std::endl;
        RRTreeNode* next_node = new RRTreeNode(next_point);
        // add next_node into RRT
        next_node->parent_node = nearest_node;
        nearest_node->children_node.push_back(next_node);
        std::cout<<"Successfully Add a new node to RRT"<<std::endl;
        
        // try to connect RRT and end point
        if(node_num % kStepToCheckReachEndPoint == 0){
            std::cout<<"Try to connect End point! Node Num:"<<node_num<<std::endl;
            RRTreeNode* node = NearestRRTreeNode(root,end_point);
            if(IsPathLocalFree(node->point,end_point,submap_indexes)){
                // find the path!
                while(node!=nullptr){
                    path.insert(path.begin(),node->point);
                    node = node->parent_node;
                }
                path.push_back(end_point);
                std::cout<<"Successfully find a path!"<<std::endl;
                DestroyRRTree(root);
                return path;
            }
        }
    }
    std::cout<<"Fail to find a path from submap!"<<std::endl;
    DestroyRRTree(root);
    return {};
}
    
// Return a path bewteen origins of two connecting submaps
Path NavigationNode::PlanPathRRT(SubmapIndex start_idx,
                                 SubmapIndex end_idx){
    std::cout<<"Try to connect two submaps:"<<start_idx<<","<<end_idx<<std::endl;
    geometry_msgs::Point start_point = submap_[start_idx].pose.position;
    geometry_msgs::Point end_point = submap_[end_idx].pose.position;
    std::vector<SubmapIndex> submap_indexs = {start_idx,end_idx};
    
    // Naively check the straight line between two points
    if(IsPathLocalFree(start_point,end_point,submap_indexs)){
        std::cout<<"Directly connect two submaps: "<<start_idx<<","<<end_idx<<std::endl;
        return {start_point,end_point};
    }
    std::vector<SubmapIndex> submap_indexes = {start_idx,end_idx};
    return LocalPlanPathRRT(start_point, end_point, submap_indexes);
}

    

// Return whether a straight line between two points are free
bool NavigationNode::IsPathLocalFree(const geometry_msgs::Point& start,
                                     const geometry_msgs::Point& end,
                                     const std::vector<SubmapIndex>& submap_indexs){
    std::cout<<"Begin Check Local Path"<<std::endl;
    float distance2 = Distance2BetweenPoint(start,end);
    float step = kOccupyGridResolution / sqrt(distance2);
    for(float i=0;i<=1;i+=step){
        geometry_msgs::Point point = (1.0-i) * start + i * end;
        bool is_free = false;
        for(auto submap_index:submap_indexs){
            int val = IsLocalFree(point, submap_index);
            if(val>=kOccupyThreshhold) return false;
            if(val!=-1){
                is_free = true;
                break;
            }
        }
        if(!is_free) return false;
    }
    return true;
}
    
// add new entry to road map
void NavigationNode::AddRoadMapEntry(const SubmapIndex submap_index){
    // check version
    std::cout<<"Try to add sumbap into road map "<<submap_index<<std::endl;
    auto& submap_entry = submap_[submap_index];
    if(submap_entry.submap_version!=kFinishVersion) return;
    
    // go throush existing
    for(auto& pair:submap_){
        // check validation of other_submap_entry
        auto& other_submap_entry = pair.second;
        if(other_submap_entry.submap_version!=kFinishVersion) continue;
        if(other_submap_entry.submap_index==submap_entry.submap_index) continue;
        
        float distance2 = Distance2BetweenPose(submap_entry.pose,other_submap_entry.pose);
        if(distance2<kDistance2ThresholdForUpdating || 
            other_submap_entry.submap_index-submap_entry.submap_index==1 ||
            other_submap_entry.submap_index-submap_entry.submap_index==-1){
            // TODO: Try to connect these two submap
            // use RRT to connect these two submap
            Path path = PlanPathRRT(submap_entry.submap_index,other_submap_entry.submap_index);
            if(path.empty()) continue;
            
            // add into road_map
            SubmapConnectState submap_connect_state (submap_entry.submap_index,
                                                     other_submap_entry.submap_index,
                                                     distance2);
            SubmapConnectState other_submap_connect_state (other_submap_entry.submap_index,
                                                           submap_entry.submap_index,
                                                           distance2);
            
            submap_connect_state.path = path;
            std::reverse(path.begin(),path.end());
            other_submap_connect_state.path = std::move(path);
            road_map_[submap_entry.submap_index][other_submap_entry.submap_index] = std::move(submap_connect_state);
            road_map_[other_submap_entry.submap_index][submap_entry.submap_index] = std::move(other_submap_connect_state);
        }
    }
}
    
// update the road mao every time received submaplist
void NavigationNode::UpdateRoadMap(const cartographer_ros_msgs::SubmapList::ConstPtr& msg){
    ::cartographer::common::MutexLocker locker(&mutex_);
    
    // check if submap has been changed
    //std::cout<<"Update RoadMap"<<std::endl;
    for(auto& submap_entry:msg->submap){
        SubmapIndex submap_index = submap_entry.submap_index;
        // ignore unfinished submaps
        if(submap_entry.submap_version<kFinishVersion) continue;
        
        // New submap
        if(submap_.find(submap_index)==submap_.end()){
            submap_[submap_index] = submap_entry;
            AddSubmapGrid(submap_index);
            AddRoadMapEntry(submap_index);
        } else{
            // Existing submap
            auto& old_submap_entry = submap_[submap_index];
            // check how much the submap_entry has changed
            float distance2 = Distance2BetweenPose(old_submap_entry.pose,submap_entry.pose);
            float rotation = RotationBetweenPose(old_submap_entry.pose,submap_entry.pose);
            if( rotation>kRotationThresholdForUpdating || distance2>kDistance2ThresholdForUpdating){
                submap_[submap_index] = submap_entry;
                AddSubmapGrid(submap_index);
                AddRoadMapEntry(submap_index);
            }
        }
    }
    //PrintState();
}

/**
Functions below are for test
*/

void NavigationNode::IsClickedPointFree(const geometry_msgs::PointStamped::ConstPtr& msg){
    //std::cout<<msg->point.x<<","<<msg->point.y<<":";
    IsLocalFree(msg->point,0) ? std::cout<<"Free" : std::cout<<"Occupied";
    std::cout<<std::endl;
}

void NavigationNode::NavigateToClickedPoint(const geometry_msgs::PointStamped::ConstPtr& msg){
    std::cout<<"Received Goal:"<<msg->point.x<<","<<msg->point.y<<std::endl;
    geometry_msgs::Point departure;
    departure.x = 0.01;
    departure.y = 0.01;
    departure.z = 0.0;
    if(PlanPathRRT(departure,msg->point).empty()){
        std::cout<<"Fail to find a valid path!"<<std::endl;
    }
}
    
// print out the current state for testing and debugging
void NavigationNode::PrintState(){
    // print out submap_
    for(const auto& submap_entry:submap_){
        std::cout<<submap_entry.first<<": "
        <<submap_entry.second.pose.position.x<<","
        <<submap_entry.second.pose.position.y<<std::endl;
    }
    // print out road_map_
    for(auto pair:road_map_){
        std::cout<<pair.first<<": ";
        for(auto connection:pair.second){
            std::cout<<connection.first<<" ";
        }
        std::cout<<std::endl;
    }
}

void NavigationNode::AddDisplayPath(Path path){
    path_to_display_.header.stamp = ::ros::Time::now();
    path_to_display_.header.frame_id = "/map";
    path_to_display_.poses.clear();
    for(auto& point : path){
        geometry_msgs::PoseStamped pose_stamped;
        pose_stamped.header.stamp = ::ros::Time::now();
        pose_stamped.header.frame_id = "/map";
        pose_stamped.pose.position = std::move(point);
        pose_stamped.pose.orientation.w = 1.0;
        pose_stamped.pose.orientation.x = 1.0;
        pose_stamped.pose.orientation.y = 1.0;
        pose_stamped.pose.orientation.z = 1.0;
        path_to_display_.poses.push_back(std::move(pose_stamped));
    }
}
    
void NavigationNode::PublishPath(const ::ros::WallTimerEvent& timer_event){
    path_publisher_.publish(path_to_display_);
}
    


} // namespace cartographer_ros_navigation
} // namespace cartographer_ros
