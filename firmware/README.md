# 🔧 Firmware Directory

This directory contains the firmware files for the ESP32 flight controller system, organized by stability and development status.

## 📁 Directory Structure

### ✅ **Stable Versions** (Production Ready)

#### 🚁 `/stable_drone`

**Production drone flight controller firmware:**

- `droneFreeRTOS.ino` - **✅ STABLE PRODUCTION FIRMWARE**
  - Complete RF remote control with ESC integration
  - FreeRTOS multi-threading architecture (50Hz motor updates)
  - Comprehensive sensor integration (BME280, GPS, environmental sensors)
  - Safety systems (arming, emergency stop, control timeout)
  - ACK payload telemetry system
  - **Status:** Fully tested and operational

#### 🎮 `/stable_remote`

**Production remote controller firmware:**

- `remoteControllerStable.ino` - **✅ STABLE PRODUCTION REMOTE**
  - Dual joystick support with toggle switches
  - 5Hz control transmission with telemetry display
  - Emergency stop and arming controls
  - **Status:** Fully tested and operational
- `FastControlRemote.ino` - Alternative stable remote implementation

### 🔬 **Development Versions** (Active Development)

#### 🚁 `/development_drone`

**Development drone flight controller firmware:**

- `droneFreeRTOS.ino` - **🔬 DEVELOPMENT VERSION**
  - Copy of stable firmware for PID integration development
  - **Next Phase:** MPU6050 PID stabilization implementation
  - **Status:** Ready for PID controller development

#### 🎮 `/development_remote`

**Development remote controller firmware:**

- `remoteControllerStable.ino` - **🔬 DEVELOPMENT VERSION**
  - Copy of stable firmware for enhanced features
  - **Next Phase:** Advanced control modes and parameter tuning
  - **Status:** Ready for feature development
- `FastControlRemote.ino` - Alternative development remote

## 🚀 Getting Started

### For Production Use (Stable)

1. **For Drone**: Upload `stable_drone/droneFreeRTOS.ino` to the ESP32 flight controller
2. **For Remote**: Upload `stable_remote/remoteControllerStable.ino` to the ESP32 remote controller
3. **Configure**: Ensure matching RF24 channel settings (Channel 76)
4. **Test**: Verify communication and control response before flight

### For Development (Experimental)

1. **For Drone Development**: Upload `development_drone/droneFreeRTOS.ino` for PID implementation
2. **For Remote Development**: Upload `development_remote/remoteControllerStable.ino` for feature testing
3. **Backup**: Always keep stable versions as backup
4. **Test Thoroughly**: Extensive testing required before flight operations

## 📋 Version Management

- **Stable Versions**: Tested, reliable firmware for production use
- **Development Versions**: Active development for new features (PID stabilization)
- **Backup Strategy**: Always maintain working stable versions
- **Update Process**: Test development → Validate → Promote to stable

## ⚠️ Safety Notes

- **Use stable versions for actual flight operations**
- Always test firmware changes in a safe environment
- Verify all sensor readings before flight operations
- Ensure emergency stop functionality is working
- Keep backup copies of working firmware versions
