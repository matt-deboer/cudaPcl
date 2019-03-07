/* Copyright (c) 2014, Julian Straub <jstraub@csail.mit.edu>
 * Licensed under the MIT license. See the license file LICENSE.
 */

#pragma once

#include <iostream>
#include <sstream>

#include <pcl/io/openni_grabber.h>
#include <pcl/io/openni_camera/openni_depth_image.h>
#include <pcl/io/openni_camera/openni_image.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
// #include <opencv2/contrib/contrib.hpp>

#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>

#include <cudaPcl/openniGrabber.hpp>

#define USE_PCL_VIEWER
//#include <pcl/point_cloud.h>
//#include <pcl/impl/point_types.hpp>
#include <pcl/common/common_headers.h>
#ifdef USE_PCL_VIEWER
  #pragma GCC system_header
  #include <pcl/visualization/cloud_viewer.h>
#endif

using std::cout;
using std::endl;


namespace cudaPcl {

/*
 * OpenniVisualizer visualizes the RGB and depth frame and adds a visualizer
 * for a point-cloud using pcl (but does not display anything). 
 *
 * Importantly all visualization is handled in a separate thread.
 */
class OpenniVisualizer : public OpenniGrabber
{
public:
  OpenniVisualizer(bool visualizeCloud = true) : OpenniGrabber(),
    visualizeCloud_(visualizeCloud), update_(false), pc_(new
        pcl::PointCloud<pcl::PointXYZRGB>(1,1))
  {};

  virtual ~OpenniVisualizer() 
  {};

  virtual void depth_cb(const uint16_t * depth, uint32_t w, uint32_t h)
  {
    cv::Mat dMap = cv::Mat(h,w,CV_16U,const_cast<uint16_t*>(depth));
    {
      boost::mutex::scoped_lock updateLock(updateModelMutex);
      dColor_ = colorizeDepth(dMap, 30.,4000.);
      d_ = dMap.clone();
    }
    this->update();
  };  

  static cv::Mat colorizeDepth(const cv::Mat& dMap, float min, float max);
  static cv::Mat colorizeDepth(const cv::Mat& dMap);
  virtual void run();

protected:
  bool visualizeCloud_;
  bool update_;
  boost::mutex updateModelMutex;
  cv::Mat dColor_;
  cv::Mat d_;
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr pc_;

  void update(){
    boost::mutex::scoped_lock updateLock(updateModelMutex);
    update_=true;
  };

  virtual void visualize_(char key);
  virtual void visualizeRGB();
  virtual void visualizeD();
  virtual void visualizePC();

  virtual void rgb_cb_ (const boost::shared_ptr<openni_wrapper::Image>& rgb)
  {
    // overwrite to use the lock to update rgb;
    int w = rgb->getWidth(); 
    int h = rgb->getHeight(); 
    // TODO: uggly .. but avoids double copy of the image.
    boost::mutex::scoped_lock updateLock(updateModelMutex);
    if(this->rgb_.cols < w) {
      this->rgb_ = cv::Mat(h,w,CV_8UC3);
    }
    cv::Mat Irgb(h,w,CV_8UC3);
    rgb->fillRGB(w,h,Irgb.data);
    cv::cvtColor(Irgb, this->rgb_,CV_BGR2RGB);
    rgb_cb(this->rgb_.data,w,h);
  }

#ifdef USE_PCL_VIEWER
  boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer_;
#endif
private:
  void visualizerThread();
};

// ------------------------------------ impl ---------------------------------

void OpenniVisualizer::visualize_(char key)
{
  static int64_t frameId = 0;
  visualizeRGB();
  visualizeD();
  visualizePC();
  if (key == 's') {
    std::cout << "key: " << key << std::endl;
    std::stringstream ss;
    ss << "./frame_" << std::setfill('0') << std::setw(9) << frameId << "_";
    std::string prefix = ss.str();
    if (this->rgb_.rows > 0 && this->rgb_.cols > 0)
      cv::imwrite(prefix+std::string("_rgb.png"),this->rgb_);
    if (dColor_.rows > 0 && dColor_.cols > 0) {
      cv::imwrite(prefix+std::string("_d.png"), d_);
      std::cout<<d_.rows << "x"<<d_.cols<<std::endl;
    }
  }
  ++frameId;
};

void OpenniVisualizer::visualizeRGB()
{
  if (this->rgb_.rows > 0 && this->rgb_.cols > 0)
    cv::imshow("rgb",this->rgb_);
};

void OpenniVisualizer::visualizeD()
{
  if (dColor_.rows > 0 && dColor_.cols > 0)
    cv::imshow("d",dColor_);
};

void OpenniVisualizer::visualizePC()
{
  if (!visualizeCloud_) return;
#ifdef USE_PCL_VIEWER
  if(!viewer_->updatePointCloud(pc_, "pc"))
    viewer_->addPointCloud(pc_, "pc");
#endif
}

void OpenniVisualizer::visualizerThread()
{
#ifdef USE_PCL_VIEWER
  if (visualizeCloud_) {
    // prepare visualizer named "viewer"
    viewer_ = boost::shared_ptr<pcl::visualization::PCLVisualizer>(
        new pcl::visualization::PCLVisualizer ("3D Viewer"));
    viewer_->initCameraParameters ();
    viewer_->setBackgroundColor (255, 255, 255);
    viewer_->addCoordinateSystem (1.0);
    viewer_->setSize(1000,1000);

    while (!viewer_->wasStopped ()) {
      viewer_->spinOnce (10);
      char key = cv::waitKey(10);
      // Get lock on the boolean update and check if cloud was updated
      boost::mutex::scoped_lock updateLock(updateModelMutex);
      if (update_) {
        visualize_(key);
        update_ = false;
      }
    }
  } else {
    while (42) {
      char key = cv::waitKey(10);
      // Get lock on the boolean update and check if cloud was updated
      boost::mutex::scoped_lock updateLock(updateModelMutex);
      if (update_) {
        visualize_(key);
        update_ = false;
      }
    }
  }
#else
  while (42)
  {
    char key = cv::waitKey(10);
    // Get lock on the boolean update and check if cloud was updated
    boost::mutex::scoped_lock updateLock(updateModelMutex);
    if (update_)
    {
      visualize_(key);
      update_ = false;
    }
  }
#endif
}

void OpenniVisualizer::run ()
{
  boost::thread visualizationThread(&OpenniVisualizer::visualizerThread,this); 
  this->run_impl();
  while (42) boost::this_thread::sleep (boost::posix_time::seconds (1));
  this->run_cleanup_impl();
  visualizationThread.join();
};

cv::Mat OpenniVisualizer::colorizeDepth(const cv::Mat& dMap, float min,
    float max)
{
//  double Min,Max;
//  cv::minMaxLoc(dMap,&Min,&Max);
//  cout<<"min/max "<<min<<" " <<max<<" actual min/max "<<Min<<" " <<Max<<endl;
  cv::Mat d8Bit = cv::Mat::zeros(dMap.rows,dMap.cols,CV_8UC1);
  cv::Mat dColor;
  dMap.convertTo(d8Bit,CV_8UC1, 255./(max-min));
  cv::applyColorMap(d8Bit,dColor,cv::COLORMAP_JET);
  return dColor;
}

cv::Mat OpenniVisualizer::colorizeDepth(const cv::Mat& dMap)
{
  double min,max;
  cv::minMaxLoc(dMap,&min,&max);
//  cout<<" computed actual min/max "<<min<<" " <<max<<endl;
  cv::Mat d8Bit = cv::Mat::zeros(dMap.rows,dMap.cols,CV_8UC1);
  cv::Mat dColor;
  dMap.convertTo(d8Bit,CV_8UC1, 255./(max-min));
  cv::applyColorMap(d8Bit,dColor,cv::COLORMAP_JET);
  return dColor;
}
}
