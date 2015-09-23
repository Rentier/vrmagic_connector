#include <iostream>
#include <cstdlib>
#include <string>

#include <ros/ros.h>
#include <ros/console.h>

#include "camera_handle.hpp"

namespace vrmagic {

static const VRmColorFormat TARGET_COLOR_FORMAT = VRM_BGR_3X8;

// Macros

#define VRM_CHECK(C)                                              \
  if (!(C)) {                                                     \
    ROS_ERROR("%s : Line %d", VRmUsbCamGetLastError(), __LINE__); \
    exit(EXIT_FAILURE);                                           \
  }

// Helper functions

static VRmPropId portnumToPropId(VRmDWORD port) {
  switch (port) {
    case 1:
      return VRM_PROPID_GRAB_SENSOR_PROPS_SELECT_1;
    case 2:
      return VRM_PROPID_GRAB_SENSOR_PROPS_SELECT_2;
    case 3:
      return VRM_PROPID_GRAB_SENSOR_PROPS_SELECT_3;
    case 4:
      return VRM_PROPID_GRAB_SENSOR_PROPS_SELECT_4;
    default:
      ROS_FATAL("Cannot convert port to prop id: %d", port);
  }
}

void cameraShutdown() { VRmUsbCamCleanup(); }

// Member functions

CameraHandle::CameraHandle(Config conf) {
  if (conf.enableLogging) VRmUsbCamEnableLogging();

  this->conf = conf;

  initCamera();
  startCamera();
}

CameraHandle::~CameraHandle() {
  VRmUsbCamStop(device);
  VRmUsbCamCloseDevice(device);
  VRmUsbCamCleanup();
}

void CameraHandle::initCamera() {
  // Scanning for VRMagic devices, opening the first one to find.
  // If a device is found, it is opened.
  openDevice();

  // Activate the sensors of desire
  setSensorActive(conf.portLeft);
  setSensorActive(conf.portRight);

  // Get source format of the camera
  getSourceFormat();

  // Select a target format from the list of formats. The source image grabbed from the camera
  // will be converted to this format if possible.
  setTargetFormat();

  // Setting the properties of the camera according to the configuration
  setProperties();
}

void CameraHandle::openDevice() {
  VRmDWORD libversion;
  VRM_CHECK(VRmUsbCamGetVersion(&libversion));
  ROS_INFO("VR Magic lib has version %d", libversion);

  VRmDWORD size = 0;

  ROS_INFO("Scanning for devices");
  VRM_CHECK(VRmUsbCamGetDeviceKeyListSize(&size));
  ROS_INFO("Found %d devices", size);

  device = 0;
  VRmDeviceKey* devKey = 0;
  for (VRmDWORD i = 0; i < size && !device; ++i) {
    VRM_CHECK(VRmUsbCamGetDeviceKeyListEntry(i, &devKey));
    if (!devKey->m_busy) {
      VRM_CHECK(VRmUsbCamOpenDevice(devKey, &device));
      ROS_INFO("Found device: %s [%s]", devKey->mp_product_str, devKey->mp_manufacturer_str);
    }
    VRM_CHECK(VRmUsbCamFreeDeviceKey(&devKey));
  }

  ROS_INFO("Trying to open device");

  if (!device) {
    ROS_FATAL("No suitable VRmagic device found!");
    exit(-1);
  }

  ROS_INFO("Device opened");
}

/**
 * When setting properties, the sensor for which the properties should be set first has to
 * be activated with this function. Call it before setting parameters.
 * @param port The port number of the sensor
 */
void CameraHandle::setSensorActive(VRmDWORD port) {
  VRmPropId sensorProp = portnumToPropId(port);
  VRmUsbCamSetPropertyValueE(device, VRM_PROPID_GRAB_SENSOR_PROPS_SELECT_E, &sensorProp);
}

void CameraHandle::getSourceFormat() {
  VRmImageFormat sourceFormat;

  VRM_CHECK(VRmUsbCamGetSourceFormatEx(device, conf.portLeft, &sourceFormat));

  const char* source_color_format_str;
  VRM_CHECK(VRmUsbCamGetStringFromColorFormat(sourceFormat.m_color_format, &source_color_format_str));

  ROS_INFO("Selected source format: %d x %d (%s)",
           sourceFormat.m_width,
           sourceFormat.m_height,
           source_color_format_str);
}

void CameraHandle::setTargetFormat() {
  VRmDWORD numberOfTargetFormats, i;
  VRM_CHECK(VRmUsbCamGetTargetFormatListSizeEx2(device, conf.portLeft, &numberOfTargetFormats));
  for (i = 0; i < numberOfTargetFormats; ++i) {
    VRM_CHECK(VRmUsbCamGetTargetFormatListEntryEx2(device, conf.portLeft, i, &targetFormat));
    if (targetFormat.m_color_format == TARGET_COLOR_FORMAT) break;
  }

  // Check for right target format
  if (targetFormat.m_color_format != TARGET_COLOR_FORMAT) {
    const char* screen_color_format_str;
    VRM_CHECK(VRmUsbCamGetStringFromColorFormat(TARGET_COLOR_FORMAT, &screen_color_format_str));
    ROS_FATAL("%s not found in target format list.", screen_color_format_str);
    exit(-1);
  }

  const char* targetColorFormatStr;
  VRM_CHECK(VRmUsbCamGetStringFromColorFormat(targetFormat.m_color_format, &targetColorFormatStr));
  ROS_INFO("Selected target format: %d x %d (%s)",
           targetFormat.m_width,
           targetFormat.m_height,
           targetColorFormatStr);
}

void CameraHandle::setProperties() {
  checkAndSanitizeConfig();

  if (conf.setGain) setGain();
  if (conf.setExposure) setExposure();
}

void CameraHandle::setGain() {
  setPropertyLeftAndRight(conf.gainLeft, conf.gainRight, VRM_PROPID_CAM_GAIN_MONOCHROME_I);
}

void CameraHandle::setExposure() {
  ROS_INFO("Set exposure");
  setPropertyLeftAndRight(conf.exposureLeft, conf.exposureRight, VRM_PROPID_CAM_EXPOSURE_TIME_F);
}

template <typename T>
void CameraHandle::setPropertyLeftAndRight(T valueLeft, T valueRight, VRmPropId property) {
  setSingleProperty(valueLeft, property, conf.portLeft);
  setSingleProperty(valueRight, property, conf.portRight);
}

void CameraHandle::setSingleProperty(int value, VRmPropId property, VRmDWORD port) {
  VRmBOOL supported;
  VRmPropInfo propInfo;

  setSensorActive(port);

  VRM_CHECK(VRmUsbCamGetPropertySupported(device, property, &supported));

  if (supported) {
    VRM_CHECK(VRmUsbCamSetPropertyValueI(device, property, &value));
    VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));
    ROS_INFO("%s changed to: %i", propInfo.m_description, value);
  } else {
    VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));
    ROS_WARN("Property '%s' not supported!", propInfo.m_description);
  }
}

void CameraHandle::setSingleProperty(float value, VRmPropId property, VRmDWORD port) {
  VRmBOOL supported;
  VRmPropInfo propInfo;

  setSensorActive(port);

  VRM_CHECK(VRmUsbCamGetPropertySupported(device, property, &supported));

  if (supported) {
    VRM_CHECK(VRmUsbCamSetPropertyValueF(device, property, &value));
    VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));
    ROS_INFO("%s changed to: %f ms", propInfo.m_description, value);
  } else {
    VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));
    ROS_WARN("Property '%s' not supported!", propInfo.m_description);
  }
}

void CameraHandle::setSingleProperty(bool value, VRmPropId property, VRmDWORD port) {
  VRmBOOL supported;
  VRmPropInfo propInfo;
  unsigned int valAsInt = value;

  setSensorActive(port);

  VRM_CHECK(VRmUsbCamGetPropertySupported(device, property, &supported));

  if (supported) {
    VRM_CHECK(VRmUsbCamSetPropertyValueB(device, property, &valAsInt));
    VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));
    ROS_INFO("%s changed to: %d ms", propInfo.m_description, value);
  } else {
    VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));
    ROS_WARN("Property '%s' not supported!", propInfo.m_description);
  }
}

// To check the configuration, the device has to be opnened before. That is because some properties
// are platform dependent. If parameters are not in the right range, the default will be used. If a
// parameter is not writeable, it will not be set.
void CameraHandle::checkAndSanitizeConfig() {
  // Check gain
  checkAndSanitizeProperty(conf.gainLeft, VRM_PROPID_CAM_GAIN_MONOCHROME_I, "gainLeft");
  checkAndSanitizeProperty(conf.gainRight, VRM_PROPID_CAM_GAIN_MONOCHROME_I, "gainRight");

  // Check exposure
  checkAndSanitizeProperty(conf.exposureLeft, VRM_PROPID_CAM_EXPOSURE_TIME_F, "exposureLeft");
  checkAndSanitizeProperty(conf.exposureRight, VRM_PROPID_CAM_EXPOSURE_TIME_F, "exposureRight");
}

/**
 * [CameraHandle::checkAndSanitizeProperty description]
 * @param value    [description]
 * @param property [description]
 * @param name     [description]
 */
void CameraHandle::checkAndSanitizeProperty(int& value, VRmPropId property, std::string name) {
  VRmPropInfo propInfo;
  VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));

  if (VRM_PROP_TYPE_INT != propInfo.m_type) {
    ROS_ERROR("Invalid type of property!");
  }

  VRmPropAttribsI attribs;
  VRmUsbCamGetPropertyAttribsI(device, property, &attribs);
  if (value < attribs.m_min || value > attribs.m_max) {
    ROS_WARN("Invalid value for parameter %s, has to be in [%d,%d], but was: %d",
             name.c_str(),
             attribs.m_min,
             attribs.m_max,
             value);

    ROS_WARN("Default will be used for %s: %d", name.c_str(), attribs.m_default);
    value = attribs.m_default;
  }
}

void CameraHandle::checkAndSanitizeProperty(float& value, VRmPropId property, std::string name) {
  VRmPropInfo propInfo;
  VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));

  if (VRM_PROP_TYPE_FLOAT != propInfo.m_type) {
    ROS_ERROR("Invalid type of property!");
  }

  VRmPropAttribsF attribs;
  VRmUsbCamGetPropertyAttribsF(device, property, &attribs);
  if (value < attribs.m_min || value > attribs.m_max) {
    ROS_WARN("Invalid value for parameter %s, has to be in [%f,%f], but was: %f",
             name.c_str(),
             attribs.m_min,
             attribs.m_max,
             value);

    ROS_WARN("Default will be used for %s: %f", name.c_str(), attribs.m_default);
    value = attribs.m_default;
  }
}

void CameraHandle::checkAndSanitizeProperty(bool& value, VRmPropId property, std::string name) {
  VRmPropInfo propInfo;
  VRM_CHECK(VRmUsbCamGetPropertyInfo(device, property, &propInfo));

  if (VRM_PROP_TYPE_BOOL != propInfo.m_type) {
    ROS_ERROR("Invalid type of property!");
  }

  VRmPropAttribsB attribs;
  VRmUsbCamGetPropertyAttribsB(device, property, &attribs);
  if (value < attribs.m_min || value > attribs.m_max) {
    ROS_WARN("Invalid value for parameter %s, has to be in [%d,%d], but was: %d",
             name.c_str(),
             attribs.m_min,
             attribs.m_max,
             value);

    ROS_WARN("Default will be used for %s: %s", name.c_str(), attribs.m_default ? "true" : "false");
    value = attribs.m_default;
  }
}

void CameraHandle::startCamera() {
  ROS_INFO("Starting the camera.");

  VRM_CHECK(VRmUsbCamResetFrameCounter(device));
  VRM_CHECK(VRmUsbCamStart(device));

  ROS_INFO("Beginning to grab.");
}

void CameraHandle::grabFrameLeft(sensor_msgs::Image& img, const ros::Time& triggerTime) {
  grabFrame(conf.portLeft, img, triggerTime);
}

void CameraHandle::grabFrameRight(sensor_msgs::Image& img, const ros::Time& triggerTime) {
  grabFrame(conf.portRight, img, triggerTime);
}

void CameraHandle::grabFrame(VRmDWORD port, sensor_msgs::Image& img, const ros::Time& triggerTime) {
  VRmImage* sourceImg = 0;
  VRmDWORD framesDropped = 0;

  if (VRmUsbCamLockNextImageEx2(device, port, &sourceImg, &framesDropped, conf.timeout)) {
    VRmImage* targetImage = 0;
    VRM_CHECK(VRmUsbCamNewImage(&targetImage, targetFormat));
    VRM_CHECK(VRmUsbCamConvertImage(sourceImg, targetImage));

    // Fill in the image message with the converted frame from the camera
    img.width = targetImage->m_image_format.m_width;
    img.height = targetImage->m_image_format.m_height;
    img.step = img.width * 3;  // width * byte per pixel
    img.encoding = sensor_msgs::image_encodings::BGR8;
    img.data.resize(img.height * img.step);
    img.header.stamp = triggerTime;
    img.header.frame_id = conf.frameId;

    // Convert from strided image to rectangular
    for (unsigned int y = 0; y < img.height; y++) {
      for (unsigned int x = 0; x < img.width * 3; x++) {
        img.data[y * img.width * 3 + x] = targetImage->mp_buffer[y * targetImage->m_pitch + x];
      }
    }

    VRM_CHECK(VRmUsbCamFreeImage(&targetImage));

    VRM_CHECK(VRmUsbCamUnlockNextImage(device, &sourceImg));
  } else {
    ROS_FATAL("Could not lock image: %s", VRmUsbCamGetLastError());
  }
}
}
