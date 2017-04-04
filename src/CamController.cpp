/*
 * 2016 Bernd Pfrommer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <poll_cameras/CamController.h>
#include <math.h>

#define THREAD_GUARD 0

namespace poll_cameras {
  CamController::CamController(const ros::NodeHandle& parentNode, int numCameras)
    : parentNode_(parentNode) {
    configServer_.reset(new dynamic_reconfigure::Server<Config>(parentNode_));

    numCameras_ = numCameras;

    double fps;
    parentNode.getParam("fps", fps);
    setFPS(fps);
    parentNode.getParam("exposure_value", exposureValue_);

    for (int i = 0; i<numCameras_; i++) {
      CamPtr cam_tmp = 
        boost::make_shared<Cam>(parentNode_, "cam" + std::to_string(i));
      cameras_.push_back(move(cam_tmp));
    }

    // for publishing the currently set exposure values
    configServer_->setCallback(boost::bind(&CamController::configure, this, _1, _2));
  }
  CamController::~CamController()
  {
    stopPoll();
  }

  void CamController::setFPS(double fps) {
    std::unique_lock<std::mutex> lock(timeMutex_);
    fps_ = fps;
    maxWait_ = std::chrono::nanoseconds((int64_t)(0.25 * 1e9 / fps_));
  }


  void CamController::start() {
    time_ = ros::Time::now();  
    startPoll();
  }

  
  void CamController::configure(Config& config, int level) {
    ROS_INFO("configuring server!");
    if (level < 0) {
      ROS_INFO("%s: %s", parentNode_.getNamespace().c_str(),
               "Initializing reconfigure server");
    }
    setFPS(config.fps);
    exposureValue_ = config.exposure_value;
    CamConfig cc;
    cc.fps = 10.0;
    cc.video_mode       = 23; // format7
    cc.format7_mode     = 0;
    cc.width            = config.width;
    cc.height           = config.height;
    cc.raw_bayer_output = false;
    cc.trigger_source   = -1; // free running
    cc.pixel_format     = 0; // unspecified
    cc.trigger_polarity = 0;
    cc.strobe_control   = -1;
    cc.strobe_polarity  = 0;
    cc.exposure         = true;
    cc.auto_exposure    = true;
    cc.exposure_value   = 1.35;
    cc.auto_shutter     = true;
    cc.shutter_ms       = 10.0;
    cc.auto_gain        = false;
    cc.gain_db          = 0.0;
    cc.white_balance    = false;
    cc.auto_white_balance = true;
    cc.wb_blue          = 0;
    cc.wb_red           = 0;
    cc.brightness       = 0.0;
    cc.gamma            = 0.5;
    configureCams(cc, 0);
  }

  void CamController::configureCams(CamConfig& config, int level) {
    if (level < 0) {
      ROS_INFO("%s: %s", parentNode_.getNamespace().c_str(),
               "Initializing cam reconfigure server");
    }
    ROS_INFO("configuring camera!");    
    if (stopPoll()) {
      // something was running
      configureCameras(config);
      startPoll();
    } else {
      // nothing was running
      configureCameras(config);
    }
    ROS_INFO("configuring camera done!");    
  }

  void CamController::frameGrabThread(int camIndex) {
    ROS_INFO("Starting up frame grab thread for camera %d", camIndex);

    // Grab a copy of the time, then can release the lock so that 
    // trigger can start a new frame if necessary.
    ros::Time lastTime;
    {
      std::unique_lock<std::mutex> lock(timeMutex_);
      lastTime = time_;
    }
    Time t0;
    while (ros::ok()) {
      auto image_msg = boost::make_shared<sensor_msgs::Image>();
      CamPtr curCam = cameras_[camIndex];
      t0 = ros::Time::now();
      bool ret = curCam->Grab(image_msg);
      ros::Time t1 = ros::Time::now();
      {  // this section is protected by mutex
        
        std::unique_lock<std::mutex> lock(timeMutex_);
        if (camIndex == masterCamIdx_) {
          // master camera updates the timestamp for everybody
          time_ = ros::Time::now();
          timeCV_.notify_all();
        } else {
          // slave cameras wait until the master has published
          // a new timestamp.
          while (time_ <= lastTime) {
            // lock will be free while waiting!
            if (timeCV_.wait_for(lock, maxWait_) == std::cv_status::timeout) {
              ret = false;
              std::cout << camIndex << " XXXXXXXXXXXXXXXXXXXXXXXXX timed out!" << std::endl;
            }
          }
          lastTime = time_;
        }
      }
      std::cout << camIndex << " " << time_ << " grab time: " << (t1-t0) << std::endl;
      if (ret) {
        curCam->Publish(image_msg);
      } else {
        ROS_ERROR("There was a problem grabbing a frame from cam%d", camIndex);
      }
      {
        std::unique_lock<std::mutex> lock(pollMutex_);
        if (!keepPolling_) break;
      }
    }
  }

  void CamController::configureCameras(CamConfig& config) {
    if (masterCamIdx_ >= cameras_.size()) {
      ROS_ERROR("INVALID MASTER CAM INDEX: %d for %zu cams!",
                masterCamIdx_, cameras_.size());
      return;
    }
    // first set the master!
    config.fps             = fps_;
    config.strobe_control  = 2;  // GPIO 2
    config.strobe_polarity = 0;  // low
    config.trigger_source  = -1; // free running
    config.exposure        = true;
    config.exposure_value  = exposureValue_;
    config.auto_shutter    = true;
    config.auto_gain       = true;
    
    cameras_[masterCamIdx_]->Stop();
    cameras_[masterCamIdx_]->camera().Configure(config);
    cameras_[masterCamIdx_]->Start();
    

    // Switch on trigger for slave
    config.fps              = fps_;  // this will be ignored!!!
    config.trigger_source   = 0;  // external triggered
    config.trigger_polarity = 0; // low
    config.trigger_source = 3;   // GPIO 3 (wired to GPIO 2 of master)
    config.auto_shutter   = false;
    config.auto_gain      = false;
    
    ROS_INFO("slave cams: setting shutter: %.2f", config.shutter_ms);

    for (int i=0; i<numCameras_; ++i) {
      if (i == masterCamIdx_) continue;
      CamPtr curCam = cameras_[i];
      curCam->Stop();
      curCam->camera().Configure(config);
      curCam->Start();
    }
  }

  bool CamController::startPoll() {
    ROS_INFO("starting polling");
    std::unique_lock<std::mutex> lock(pollMutex_);
    if (frameGrabThreads_.empty()) {
      keepPolling_ = true;
      for (int i = 0; i < numCameras_; ++i) {
        cameras_[i]->camera().StartCapture();
        frameGrabThreads_.push_back(
          boost::make_shared<boost::thread>(&CamController::frameGrabThread, this, i));
      }
      ROS_INFO("%zu threads started!", frameGrabThreads_.size());
      return (true);
    }
    ROS_INFO("no threads started!");
    return (false);
  }

  bool CamController::stopPoll() {
    ROS_INFO("stopping polling");
    {
      std::unique_lock<std::mutex> lock(pollMutex_);
      keepPolling_ = false;
    }
    if (!frameGrabThreads_.empty()) {
      // harvest the threads
      for (auto &th : frameGrabThreads_) {
        th->join();
      }
      frameGrabThreads_.clear();
      // must stop capture afterwards!
      for (auto &cam : cameras_) {
        cam->camera().StopCapture();
      }
      ROS_INFO("stopped running threads!");
      return (true);
    }
    ROS_INFO("nothing to stop!");
    return (false);
  }

}  // namespace poll_cameras
