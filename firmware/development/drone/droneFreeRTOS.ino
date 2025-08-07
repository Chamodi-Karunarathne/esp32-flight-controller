/*
 * DEVELOPMENT VERSION - Drone with Enhanced Sensor Suite - FreeRTOS Version
 * 🔬 DEVELOPMENT FIRMWARE FOR PID STABILIZATION INTEGRATION
 *
 * Complete telemetry system with BME280, AHT21, GPS, and simulated advanced sensors
 * Compatible with remote control system (12-byte control, 22-byte telemetry)
 *
 * DEVELOPMENT FOCUS:
 * - PID stabilization controller implementation
 * - MPU6050 sensor fusion integration
 * - Advanced flight modes development
 * - Parameter tuning and optimization
 *
 * ⚠️ EXPERIMENTAL - FOR DEVELOPMENT USE ONLY
 * Use stable_drone/droneFreeRTOS.ino for production flights
 *
 * FreeRTOS Task Architecture:
 * - SensorTask: Reads all sensors (1Hz for environmental, 10Hz for GPS)
 * - RadioTask: Handles RF24 communication (5Hz control reception, 1Hz telemetry transmission)
 * - StatusTask: Prints system status and diagnostics (0.1Hz)
 * - MotorTask: ESC control with PID integration (50Hz)
 * - IMUTask: High-frequency IMU reading and calibration (100Hz)
 * - PIDTask: PID control loop for stabilization (50Hz)
 *
 * NOTE: PID control is active but PID values are not transmitted in telemetry
 * to keep telemetry focused on sensor data only.
 */

#include <SPI.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_AHTX0.h>
#include <TinyGPS++.h>
#include <BH1750.h>
#include <ScioSense_ENS160.h>
#include <ESP32Servo.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// Pin Definitions
#define CE_PIN 4
#define CSN_PIN 5
#define BATTERY_PIN 35
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GUVA_PIN 36 // GUVA-S12SD UV sensor

// ESC Control Pins (PWM Outputs to Brushless Motors)
#define ESC1_PIN 13 // Front Right (CCW)
#define ESC2_PIN 12 // Back Right (CW)
#define ESC3_PIN 14 // Front Left (CW)
#define ESC4_PIN 27 // Back Left (CCW)

// ESC PWM Configuration
#define ESC_MIN_PULSE 1000 // Minimum pulse width (microseconds)
#define ESC_MAX_PULSE 2000 // Maximum pulse width (microseconds)
#define ESC_ARM_PULSE 1000 // Arming pulse width
#define ESC_FREQUENCY 50   // 50Hz PWM frequency for ESCs

// I2C addresses
#define BME280_ADDRESS 0x76
#define BH1750_ADDRESS 0x23
#define ENS160_ADDRESS 0x53
#define MPU6050_ADDRESS 0x68

// PID Control Configuration
#define PID_LOOP_FREQUENCY 50                       // Hz - 50Hz PID loop for smooth control
#define PID_LOOP_PERIOD (1000 / PID_LOOP_FREQUENCY) // ms

// PID Output Limits
#define PID_ROLL_PITCH_MAX 400 // ±400 microseconds for roll/pitch correction
#define PID_YAW_MAX 200        // ±200 microseconds for yaw correction
#define PID_ALTITUDE_MAX 300   // ±300 microseconds for altitude correction

// ================================
// PID TUNING PARAMETERS - Easy Adjustment Section
// ================================
// Roll PID Gains (Angle Stabilization)
const double ROLL_KP = 2.5;  // Proportional gain - increase for faster response
const double ROLL_KI = 0.1;  // Integral gain - increase to eliminate steady-state error
const double ROLL_KD = 0.15; // Derivative gain - increase for more damping

// Pitch PID Gains (Angle Stabilization)
const double PITCH_KP = 2.5;  // Proportional gain - usually same as roll
const double PITCH_KI = 0.1;  // Integral gain - usually same as roll
const double PITCH_KD = 0.15; // Derivative gain - usually same as roll

// Yaw PID Gains (Rate Control)
const double YAW_KP = 1.0;  // Proportional gain - lower than roll/pitch
const double YAW_KI = 0.05; // Integral gain - very small for yaw
const double YAW_KD = 0.05; // Derivative gain - minimal for yaw

// Altitude PID Gains (Position Control)
const double ALT_KP = 1.5; // Proportional gain for altitude hold
const double ALT_KI = 0.2; // Integral gain for altitude hold
const double ALT_KD = 0.8; // Derivative gain for altitude hold

// Advanced PID Parameters
const double MAX_ANGLE = 30.0;     // Maximum roll/pitch angle (degrees)
const double MAX_YAW_RATE = 180.0; // Maximum yaw rate (degrees/second)
const double MAX_CLIMB_RATE = 2.0; // Maximum climb rate (m/s)

// Filter Coefficients (0.0 to 1.0, higher = more filtering)
const double ROLL_FILTER = 0.1;  // Derivative filter for roll
const double PITCH_FILTER = 0.1; // Derivative filter for pitch
const double YAW_FILTER = 0.15;  // Derivative filter for yaw (more filtering)
const double ALT_FILTER = 0.05;  // Derivative filter for altitude

// Setpoint Smoothing (0.0 to 1.0, lower = more smoothing)
const double ROLL_SMOOTH = 0.05;  // Setpoint smoothing for roll
const double PITCH_SMOOTH = 0.05; // Setpoint smoothing for pitch
const double YAW_SMOOTH = 0.1;    // Setpoint smoothing for yaw
const double ALT_SMOOTH = 0.02;   // Setpoint smoothing for altitude
// ================================

// Task stack sizes
#define SENSOR_TASK_STACK 8192
#define RADIO_TASK_STACK 4096
#define STATUS_TASK_STACK 2048

// Task priorities
#define SENSOR_TASK_PRIORITY 2
#define RADIO_TASK_PRIORITY 3
#define STATUS_TASK_PRIORITY 1

RF24 radio(CE_PIN, CSN_PIN);
const byte address[6] = "00001";

// Initialize real sensors
Adafruit_BME280 bme280;
Adafruit_AHTX0 aht;
TinyGPSPlus gps;
BH1750 lightMeter;
ScioSense_ENS160 ens160;
Adafruit_MPU6050 mpu6050;

// ESC Servo objects
Servo esc1, esc2, esc3, esc4;

// Control packet (12 bytes) - Added button states and toggle switches to match remote
struct ControlPacket
{
    int16_t throttle;
    int16_t roll;
    int16_t pitch;
    int16_t yaw;
    uint8_t joy1_btn; // Joystick 1 button state (0 = pressed, 1 = released)
    uint8_t joy2_btn; // Joystick 2 button state (0 = pressed, 1 = released)
    uint8_t toggle1;  // Toggle switch 1 state (0 = off, 1 = on)
    uint8_t toggle2;  // Toggle switch 2 state (0 = off, 1 = on)
};

// Enhanced telemetry packet (22 bytes) - matches remote with lux, altitude, UV index, eCO2, and TVOC
struct TelemetryPacket
{
    int16_t temperature; // x100 - Real BME280 data
    uint16_t pressure;   // x10 - Real BME280 data
    uint8_t humidity;    // % - Real AHT21 data
    uint16_t battery;    // mV - Real battery voltage
    int16_t latitude;    // GPS latitude (simplified)
    int16_t longitude;   // GPS longitude (simplified)
    uint8_t satellites;  // GPS satellite count
    uint8_t status;      // System status
    uint16_t lux;        // Light level in lux
    int16_t altitude;    // Altitude in centimeters from BME280 (for 2 decimal precision)
    uint16_t uvIndex;    // UV index x100 from GUVA sensor
    uint16_t eCO2;       // Equivalent CO2 in ppm from ENS160
    uint16_t TVOC;       // Total VOC in ppb from ENS160
};

// Enhanced PID Controller Structure with Advanced Features
struct PIDController
{
    double setpoint;
    double smoothedSetpoint; // For setpoint smoothing
    double input;
    double filteredInput; // For derivative filtering
    double output;
    double kp, ki, kd;
    double lastInput;
    double lastFilteredInput;
    double iTerm;
    double outputMin, outputMax;
    double iTermMin, iTermMax; // Separate integral limits for anti-windup
    unsigned long lastTime;
    bool enabled;

    // Derivative filtering
    double derivativeAlpha; // Low-pass filter coefficient for derivative

    // Setpoint smoothing
    double setpointAlpha; // Smoothing factor for setpoint changes

    // Anti-windup flags
    bool wasOutputSaturated;
    bool wasIntegralSaturated;

    void initialize(double p, double i, double d, double minOut, double maxOut)
    {
        kp = p;
        ki = i;
        kd = d;
        outputMin = minOut;
        outputMax = maxOut;

        // Set integral limits to 50% of output range to prevent windup
        double range = outputMax - outputMin;
        iTermMin = outputMin + range * 0.25;
        iTermMax = outputMax - range * 0.25;

        lastInput = 0;
        lastFilteredInput = 0;
        filteredInput = 0;
        iTerm = 0;
        setpoint = 0;
        smoothedSetpoint = 0;
        lastTime = millis();
        enabled = false;
        wasOutputSaturated = false;
        wasIntegralSaturated = false;

        // Filter coefficients (higher = more filtering)
        derivativeAlpha = 0.1; // 10% new, 90% old for derivative
        setpointAlpha = 0.05;  // 5% new, 95% old for setpoint smoothing
    }

    double compute()
    {
        if (!enabled)
            return 0;

        unsigned long now = millis();
        double dt = (double)(now - lastTime) / 1000.0; // Convert to seconds for proper units

        // Only compute if sufficient time has passed (minimum 1ms)
        if (dt < 0.001)
            return output;

        // Setpoint smoothing - gradual transition to new setpoint
        smoothedSetpoint = smoothedSetpoint + setpointAlpha * (setpoint - smoothedSetpoint);

        // Input filtering for derivative term
        filteredInput = filteredInput + derivativeAlpha * (input - filteredInput);

        double error = smoothedSetpoint - input;

        // Proportional term
        double pTerm = kp * error;

        // Integral term with advanced anti-windup
        // Only accumulate integral if output is not saturated OR error is helping to reduce saturation
        bool shouldAccumulateIntegral = !wasOutputSaturated ||
                                        (wasOutputSaturated && ((error > 0 && output <= outputMin) ||
                                                                (error < 0 && output >= outputMax)));

        if (shouldAccumulateIntegral && ki != 0.0)
        {
            iTerm += ki * error * dt;

            // Clamp integral term to prevent windup
            wasIntegralSaturated = false;
            if (iTerm > iTermMax)
            {
                iTerm = iTermMax;
                wasIntegralSaturated = true;
            }
            else if (iTerm < iTermMin)
            {
                iTerm = iTermMin;
                wasIntegralSaturated = true;
            }
        }

        // Derivative term with filtered input to reduce noise
        double dInput = 0;
        if (dt > 0)
        {
            dInput = (filteredInput - lastFilteredInput) / dt;
        }
        double dTerm = -kd * dInput; // Negative because we want to reduce rate of change

        // Compute final output
        double rawOutput = pTerm + iTerm + dTerm;

        // Output saturation/clamping
        output = rawOutput;
        if (output > outputMax)
        {
            output = outputMax;
            wasOutputSaturated = true;
        }
        else if (output < outputMin)
        {
            output = outputMin;
            wasOutputSaturated = true;
        }
        else
        {
            wasOutputSaturated = false;
        }

        // Remember values for next iteration
        lastInput = input;
        lastFilteredInput = filteredInput;
        lastTime = now;

        return output;
    }

    void reset()
    {
        iTerm = 0;
        lastInput = input;
        lastFilteredInput = input;
        filteredInput = input;
        smoothedSetpoint = setpoint;
        lastTime = millis();
        wasOutputSaturated = false;
        wasIntegralSaturated = false;
    }

    void setTunings(double p, double i, double d)
    {
        // Prevent negative gains
        kp = (p < 0) ? 0 : p;
        ki = (i < 0) ? 0 : i;
        kd = (d < 0) ? 0 : d;
    }

    void setOutputLimits(double min, double max)
    {
        if (min >= max)
            return; // Invalid limits

        outputMin = min;
        outputMax = max;

        // Update integral limits
        double range = outputMax - outputMin;
        iTermMin = outputMin + range * 0.25;
        iTermMax = outputMax - range * 0.25;

        // Clamp current values if needed
        if (iTerm > iTermMax)
            iTerm = iTermMax;
        else if (iTerm < iTermMin)
            iTerm = iTermMin;

        if (output > outputMax)
            output = outputMax;
        else if (output < outputMin)
            output = outputMin;
    }

    void setDerivativeFilter(double alpha)
    {
        derivativeAlpha = constrain(alpha, 0.01, 1.0); // Limit to reasonable range
    }

    void setSetpointSmoothing(double alpha)
    {
        setpointAlpha = constrain(alpha, 0.01, 1.0); // Limit to reasonable range
    }

    void setIntegralLimits(double minPercent, double maxPercent)
    {
        double range = outputMax - outputMin;
        iTermMin = outputMin + range * constrain(minPercent, 0.0, 0.5);
        iTermMax = outputMax - range * constrain(maxPercent, 0.0, 0.5);
    }

    // Diagnostic functions
    double getProportionalTerm() { return kp * (smoothedSetpoint - input); }
    double getIntegralTerm() { return iTerm; }
    double getDerivativeTerm() { return -kd * ((filteredInput - lastFilteredInput) * 1000.0 / max(1UL, millis() - lastTime)); }
    bool isOutputSaturated() { return wasOutputSaturated; }
    bool isIntegralSaturated() { return wasIntegralSaturated; }
    double getSmoothedSetpoint() { return smoothedSetpoint; }
};

// IMU Data Structure
struct IMUData
{
    float roll, pitch, yaw;             // Euler angles in degrees
    float rollRate, pitchRate, yawRate; // Angular velocities in deg/s
    float accelX, accelY, accelZ;       // Accelerometer data in m/s²
    float gyroX, gyroY, gyroZ;          // Gyroscope data in deg/s
    float temperature;                  // IMU temperature
    bool dataValid;                     // Data validity flag
    unsigned long timestamp;            // Data timestamp
};

// Flight Mode Enumeration
enum FlightMode
{
    FLIGHT_MODE_DISARMED = 0,
    FLIGHT_MODE_MANUAL = 1,        // Direct stick control (no stabilization)
    FLIGHT_MODE_STABILIZE = 2,     // Angle stabilization (default)
    FLIGHT_MODE_ALTITUDE_HOLD = 3, // Stabilize + altitude hold
    FLIGHT_MODE_POSITION_HOLD = 4  // Full GPS position hold
};

// Advanced PID Configuration Structure
struct PIDConfig
{
    // Roll PID parameters (for roll stabilization)
    double roll_kp = ROLL_KP;         // Proportional gain from constants
    double roll_ki = ROLL_KI;         // Integral gain from constants
    double roll_kd = ROLL_KD;         // Derivative gain from constants
    double roll_filter = ROLL_FILTER; // Derivative filter coefficient
    double roll_smooth = ROLL_SMOOTH; // Setpoint smoothing

    // Pitch PID parameters (for pitch stabilization)
    double pitch_kp = PITCH_KP;         // Proportional gain from constants
    double pitch_ki = PITCH_KI;         // Integral gain from constants
    double pitch_kd = PITCH_KD;         // Derivative gain from constants
    double pitch_filter = PITCH_FILTER; // Derivative filter coefficient
    double pitch_smooth = PITCH_SMOOTH; // Setpoint smoothing

    // Yaw PID parameters (for yaw rate control)
    double yaw_kp = YAW_KP;         // Proportional gain from constants
    double yaw_ki = YAW_KI;         // Integral gain from constants
    double yaw_kd = YAW_KD;         // Derivative gain from constants
    double yaw_filter = YAW_FILTER; // More filtering for yaw (noisier)
    double yaw_smooth = YAW_SMOOTH; // More smoothing for yaw commands

    // Altitude PID parameters (for altitude hold)
    double alt_kp = ALT_KP;         // Altitude hold proportional from constants
    double alt_ki = ALT_KI;         // Integral gain from constants
    double alt_kd = ALT_KD;         // Derivative gain from constants
    double alt_filter = ALT_FILTER; // Light filtering for altitude
    double alt_smooth = ALT_SMOOTH; // Gentle altitude setpoint changes

    // Advanced tuning parameters
    double max_angle = MAX_ANGLE;           // Maximum angle setpoint from constants
    double max_yaw_rate = MAX_YAW_RATE;     // Maximum yaw rate from constants
    double max_climb_rate = MAX_CLIMB_RATE; // Maximum climb rate from constants
};

// FreeRTOS Task Handles
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t radioTaskHandle = NULL;
TaskHandle_t statusTaskHandle = NULL;
TaskHandle_t motorTaskHandle = NULL;
TaskHandle_t imuTaskHandle = NULL;
TaskHandle_t pidTaskHandle = NULL;

// FreeRTOS Synchronization
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t telemetryMutex;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t controlMutex;

// Shared data structures (protected by mutexes)
ControlPacket receivedControl;
TelemetryPacket telemetryData;
unsigned long lastControlReceived = 0;
int packetsReceived = 0;
bool sensorsInitialized = false;
bool bh1750Ready = false;
bool ens160Ready = false;

// Motor control variables
volatile bool motorsArmed = false;
volatile bool stabilizedMode = false; // Flight mode: true = Stabilized (PID), false = Manual
volatile int motorSpeeds[4] = {ESC_ARM_PULSE, ESC_ARM_PULSE, ESC_ARM_PULSE, ESC_ARM_PULSE};
unsigned long lastValidControl = 0;
#define CONTROL_TIMEOUT_MS 1000   // 1 second timeout for safety
#define MIN_THROTTLE_FOR_ARM 1100 // Minimum throttle to consider arming
#define MOTOR_TASK_STACK 4096
#define MOTOR_TASK_PRIORITY 4 // High priority for motor control

// Altitude calibration variables
#define PRESSURE_BUFFER_SIZE 20   // Increased buffer for better smoothing
float seaLevelPressure = 1013.25; // Will be calibrated at startup
bool altitudeCalibrated = false;
float pressureReadings[PRESSURE_BUFFER_SIZE]; // For smoothing pressure readings
int pressureIndex = 0;
int pressureCount = 0;

// Enhanced altitude stability
float referenceTemperature = 15.0;   // Reference temperature for compensation
float exponentialPressure = 1013.25; // Exponentially smoothed pressure
float altitudeOffset = 0.0;          // Zero-reference offset
unsigned long lastAltitudeCalibration = 0;
const float PRESSURE_SMOOTHING_ALPHA = 0.1; // Exponential smoothing factor

// Task timing variables
unsigned long sensorTaskCounter = 0;
unsigned long radioTaskCounter = 0;
unsigned long statusTaskCounter = 0;

// PID Controllers and IMU Data
PIDController rollPID, pitchPID, yawPID, altitudePID;
PIDConfig pidConfig;
IMUData imuData;
FlightMode currentFlightMode = FLIGHT_MODE_DISARMED;

// IMU Calibration Data
struct IMUCalibration
{
    float rollOffset = 0.0;
    float pitchOffset = 0.0;
    float yawOffset = 0.0;
    float gyroXOffset = 0.0;
    float gyroYOffset = 0.0;
    float gyroZOffset = 0.0;
    bool calibrated = false;
    int calibrationSamples = 0;
} imuCalibration;

// Yaw drift correction variables
float gyroZBiasSum = 0.0;
int gyroBiasSampleCount = 0;
const int GYRO_BIAS_SAMPLES = 1000; // Number of samples for bias calculation
float adaptiveGyroZBias = 0.0;      // Adaptive bias for yaw drift correction
unsigned long lastYawReset = 0;
const float YAW_LEAKY_ALPHA = 0.9999; // Leaky integrator coefficient (very close to 1)
bool isStationary = false;            // Flag to determine if drone is stationary
unsigned long stationaryStartTime = 0;
const unsigned long STATIONARY_THRESHOLD_TIME = 5000; // 5 seconds to consider stationary

// PID and Flight Control Variables
bool pidEnabled = false;
bool imuInitialized = false;
bool enableVerboseLogging = false;   // Debug verbose logging control
float targetAltitude = 0.0;          // Target altitude for altitude hold mode
float baseThrottleForHover = 1400.0; // Base throttle for hovering (will be auto-adjusted)
unsigned long lastPIDUpdate = 0;
SemaphoreHandle_t pidMutex;
SemaphoreHandle_t imuMutex;

// Enhanced motor control with PID integration
volatile float pidRollOutput = 0.0;
volatile float pidPitchOutput = 0.0;
volatile float pidYawOutput = 0.0;
volatile float pidAltitudeOutput = 0.0;

// Function declarations
void updateFlightMode();
float getFilteredAltitude();

void setup()
{
    // ⚠️ CRITICAL: ESC CALIBRATION MUST HAPPEN IMMEDIATELY ON POWER-ON
    // ESCs MUST see MAX signal (2000μs) within milliseconds of power-on to enter calibration mode

    // Configure servo library for ESCs FIRST - before anything else
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);

    // Attach ESCs to pins immediately
    esc1.setPeriodHertz(ESC_FREQUENCY);
    esc2.setPeriodHertz(ESC_FREQUENCY);
    esc3.setPeriodHertz(ESC_FREQUENCY);
    esc4.setPeriodHertz(ESC_FREQUENCY);

    bool escAttachSuccess = true;
    escAttachSuccess &= esc1.attach(ESC1_PIN, ESC_MIN_PULSE, ESC_MAX_PULSE);
    escAttachSuccess &= esc2.attach(ESC2_PIN, ESC_MIN_PULSE, ESC_MAX_PULSE);
    escAttachSuccess &= esc3.attach(ESC3_PIN, ESC_MIN_PULSE, ESC_MAX_PULSE);
    escAttachSuccess &= esc4.attach(ESC4_PIN, ESC_MIN_PULSE, ESC_MAX_PULSE);

    // IMMEDIATELY send MAX signal (2000μs) for ESC calibration - NO DELAY!
    esc1.writeMicroseconds(ESC_MAX_PULSE);
    esc2.writeMicroseconds(ESC_MAX_PULSE);
    esc3.writeMicroseconds(ESC_MAX_PULSE);
    esc4.writeMicroseconds(ESC_MAX_PULSE);

    // Now initialize serial (ESCs already have MAX signal)
    Serial.begin(115200);
    delay(1000); // Give serial time to initialize

    if (!escAttachSuccess)
    {
        Serial.println("⚠️ WARNING: Some ESCs failed to attach properly!");
    }

    Serial.println("=== Drone with Real Sensors - FreeRTOS Version ===");
    Serial.println("⚠️ ESC CALIBRATION IN PROGRESS - MAX SIGNAL ACTIVE");
    Serial.println("Waiting 2 seconds for ESC calibration...");

    // Wait exactly 2 seconds with MAX signal for ESC calibration
    delay(1000); // Additional 1 second (total 2 seconds from power-on)

    // Now send MIN signal (1000μs) to complete calibration
    esc1.writeMicroseconds(ESC_ARM_PULSE);
    esc2.writeMicroseconds(ESC_ARM_PULSE);
    esc3.writeMicroseconds(ESC_ARM_PULSE);
    esc4.writeMicroseconds(ESC_ARM_PULSE);

    Serial.println("✓ ESC CALIBRATION COMPLETE - Motors ready");

    // Initialize motor speeds to safe values
    for (int i = 0; i < 4; i++)
    {
        motorSpeeds[i] = ESC_ARM_PULSE;
    }
    motorsArmed = false;

    Serial.println("Initializing system components...");

    // Initialize I2C for sensors
    Wire.begin(21, 22); // SDA=21, SCL=22
    delay(100);

    // Initialize pins
    pinMode(BATTERY_PIN, INPUT);
    pinMode(GUVA_PIN, INPUT); // UV sensor analog input

    // Initialize GPS (Serial2 on ESP32)
    Serial2.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    // Create FreeRTOS mutexes
    i2cMutex = xSemaphoreCreateMutex();
    telemetryMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    controlMutex = xSemaphoreCreateMutex();
    pidMutex = xSemaphoreCreateMutex();
    imuMutex = xSemaphoreCreateMutex();

    if (i2cMutex == NULL || telemetryMutex == NULL || serialMutex == NULL ||
        controlMutex == NULL || pidMutex == NULL || imuMutex == NULL)
    {
        Serial.println("ERROR: Failed to create mutexes!");
        while (1)
            delay(1000);
    }

    // Initialize enhanced stability variables
    exponentialPressure = seaLevelPressure;
    altitudeOffset = 0.0;
    adaptiveGyroZBias = 0.0;
    gyroZBiasSum = 0.0;
    gyroBiasSampleCount = 0;
    isStationary = false;
    lastYawReset = millis();
    lastAltitudeCalibration = millis();

    Serial.println("✓ Enhanced stability systems initialized");

    // Initialize real sensors
    initializeRealSensors();

    // Initialize IMU and PID controllers
    initializeIMUAndPID();

    // Initialize radio with PROVEN configuration
    initializeRadio();

    // Initialize telemetry data with safe defaults
    initializeTelemetryData();

    Serial.println("Creating FreeRTOS tasks...");

    // Create FreeRTOS tasks
    xTaskCreatePinnedToCore(
        sensorTask,
        "SensorTask",
        SENSOR_TASK_STACK,
        NULL,
        SENSOR_TASK_PRIORITY,
        &sensorTaskHandle,
        0 // Core 0
    );

    xTaskCreatePinnedToCore(
        radioTask,
        "RadioTask",
        RADIO_TASK_STACK,
        NULL,
        RADIO_TASK_PRIORITY,
        &radioTaskHandle,
        1 // Core 1
    );

    xTaskCreatePinnedToCore(
        statusTask,
        "StatusTask",
        STATUS_TASK_STACK,
        NULL,
        STATUS_TASK_PRIORITY,
        &statusTaskHandle,
        0 // Core 0
    );

    xTaskCreatePinnedToCore(
        motorTask,
        "MotorTask",
        MOTOR_TASK_STACK,
        NULL,
        MOTOR_TASK_PRIORITY,
        &motorTaskHandle,
        1 // Core 1 - dedicated core for motor control
    );

    // Create IMU Task (high frequency sensor reading)
    xTaskCreatePinnedToCore(
        imuTask,
        "IMUTask",
        4096,
        NULL,
        5, // Highest priority for IMU reading
        &imuTaskHandle,
        0 // Core 0
    );

    // Create PID Task (control loop)
    xTaskCreatePinnedToCore(
        pidControlTask,
        "PIDTask",
        4096,
        NULL,
        4, // High priority for PID control
        &pidTaskHandle,
        1 // Core 1 with motor task
    );

    // Check if tasks were created successfully
    if (sensorTaskHandle == NULL || radioTaskHandle == NULL || statusTaskHandle == NULL ||
        motorTaskHandle == NULL || imuTaskHandle == NULL || pidTaskHandle == NULL)
    {
        Serial.println("ERROR: Failed to create FreeRTOS tasks!");
        while (1)
            delay(1000);
    }

    Serial.println("✓ All FreeRTOS tasks created successfully");
    Serial.println("✓ System initialization complete");
    Serial.println("Starting multitasking operation...");
}

void loop()
{
    // FreeRTOS handles all tasks - main loop can be minimal
    // Print memory usage periodically
    static unsigned long lastMemCheck = 0;
    if (millis() - lastMemCheck > 30000) // Every 30 seconds
    {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Serial.println("\n=== FreeRTOS Task Status ===");
            Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
            Serial.printf("Sensor task stack free: %d bytes\n", uxTaskGetStackHighWaterMark(sensorTaskHandle));
            Serial.printf("Radio task stack free: %d bytes\n", uxTaskGetStackHighWaterMark(radioTaskHandle));
            Serial.printf("Status task stack free: %d bytes\n", uxTaskGetStackHighWaterMark(statusTaskHandle));
            Serial.printf("Motor task stack free: %d bytes\n", uxTaskGetStackHighWaterMark(motorTaskHandle));
            Serial.printf("Task counters - Sensor: %lu, Radio: %lu, Status: %lu\n",
                          sensorTaskCounter, radioTaskCounter, statusTaskCounter);
            Serial.println("===========================\n");
            xSemaphoreGive(serialMutex);
        }
        lastMemCheck = millis();
    }

    // Yield to FreeRTOS scheduler
    vTaskDelay(pdMS_TO_TICKS(5000)); // Sleep for 5 seconds
}

void sensorTask(void *parameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t sensorFrequency = pdMS_TO_TICKS(1000); // 1Hz for environmental sensors
    const TickType_t gpsFrequency = pdMS_TO_TICKS(100);     // 10Hz for GPS

    TickType_t lastGPSRead = 0;

    for (;;)
    {
        sensorTaskCounter++;

        // Read environmental sensors (1Hz)
        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            readEnvironmentalSensors();
            xSemaphoreGive(i2cMutex);
        }

        // Read GPS more frequently (10Hz)
        TickType_t currentTime = xTaskGetTickCount();
        if ((currentTime - lastGPSRead) >= gpsFrequency)
        {
            updateGPS();
            lastGPSRead = currentTime;
        }

        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, sensorFrequency);
    }
}

void radioTask(void *parameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t radioFrequency = pdMS_TO_TICKS(200); // 5Hz radio check frequency

    for (;;)
    {
        radioTaskCounter++;

        // Check for incoming control data
        handleControlData();

        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, radioFrequency);
    }
}

void statusTask(void *parameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t statusFrequency = pdMS_TO_TICKS(10000); // 0.1Hz (every 10 seconds)

    for (;;)
    {
        statusTaskCounter++;

        // Print system status
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(500)) == pdTRUE)
        {
            printStatus();
            xSemaphoreGive(serialMutex);
        }

        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, statusFrequency);
    }
}

void initializeRadio()
{
    Serial.println("Initializing RF24 radio...");

    SPI.begin();
    delay(100);

    if (!radio.begin() || !radio.isChipConnected())
    {
        Serial.println("ERROR: Radio initialization failed!");
        while (1)
            delay(1000);
    }

    // Use EXACT same configuration as remote
    radio.openReadingPipe(1, address);
    radio.setPALevel(RF24_PA_HIGH);  // Match remote power level
    radio.setDataRate(RF24_250KBPS); // Match remote data rate
    radio.setChannel(76);
    radio.setAutoAck(true);
    radio.setRetries(15, 15);
    radio.enableDynamicPayloads();
    radio.enableAckPayload();

    // Additional range improvements to match remote
    radio.setCRCLength(RF24_CRC_16); // Use 16-bit CRC for better error detection
    radio.setAddressWidth(5);        // Use full 5-byte addresses

    // Start listening for control data
    radio.startListening();

    Serial.println("✓ Radio configured successfully!");
    Serial.println("✓ Waiting for control data...");
}

void initializeTelemetryData()
{
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        telemetryData.temperature = 2500; // 25.00°C
        telemetryData.pressure = 10132;   // 1013.2 hPa
        telemetryData.humidity = 60;      // 60%
        telemetryData.battery = 3700;     // 3.7V
        telemetryData.latitude = 0;
        telemetryData.longitude = 0;
        telemetryData.satellites = 0;
        telemetryData.status = 0x01; // System OK
        telemetryData.lux = 500;     // 500 lux
        telemetryData.altitude = 0;  // 0.00m
        telemetryData.uvIndex = 300; // UV index 3.00
        telemetryData.eCO2 = 400;    // 400 ppm
        telemetryData.TVOC = 50;     // 50 ppb
        xSemaphoreGive(telemetryMutex);
    }
}

void initializeRealSensors()
{
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        Serial.println("Initializing real sensors...");
        xSemaphoreGive(serialMutex);
    }

    // Take I2C mutex for sensor initialization
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE)
    {
        // Scan I2C bus first
        scanI2CBus();

        // Try BME280 at common addresses
        bool bme280Found = false;
        uint8_t bmeAddresses[] = {0x76, 0x77};
        for (int i = 0; i < 2; i++)
        {
            if (bme280.begin(bmeAddresses[i]))
            {
                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    Serial.print("✓ BME280 initialized at address 0x");
                    Serial.println(bmeAddresses[i], HEX);
                    xSemaphoreGive(serialMutex);
                }

                // Configure BME280 for stable readings
                bme280.setSampling(Adafruit_BME280::MODE_FORCED,      // Use forced mode for manual readings
                                   Adafruit_BME280::SAMPLING_X16,     // Temperature oversampling
                                   Adafruit_BME280::SAMPLING_X16,     // Pressure oversampling
                                   Adafruit_BME280::SAMPLING_X16,     // Humidity oversampling
                                   Adafruit_BME280::FILTER_X16,       // IIR filter
                                   Adafruit_BME280::STANDBY_MS_1000); // 1 second standby

                sensorsInitialized = true;
                bme280Found = true;
                break;
            }
        }

        if (!bme280Found)
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✗ BME280 initialization failed - using simulated data");
                xSemaphoreGive(serialMutex);
            }
        }

        // Initialize AHT21
        if (aht.begin())
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✓ AHT21 initialized");
                xSemaphoreGive(serialMutex);
            }
        }
        else
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✗ AHT21 initialization failed - using simulated data");
                xSemaphoreGive(serialMutex);
            }
        }

        // Initialize BH1750 light sensor
        if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✓ BH1750 light sensor initialized");
                xSemaphoreGive(serialMutex);
            }
            bh1750Ready = true;
        }
        else
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✗ BH1750 initialization failed - using simulated data");
                xSemaphoreGive(serialMutex);
            }
            bh1750Ready = false;
        }

        // Initialize ENS160 air quality sensor
        if (ens160.begin())
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✓ ENS160 air quality sensor initialized");
                xSemaphoreGive(serialMutex);
            }
            ens160.setMode(ENS160_OPMODE_STD);
            ens160Ready = true;
        }
        else
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✗ ENS160 initialization failed - using simulated data");
                xSemaphoreGive(serialMutex);
            }
            ens160Ready = false;
        }

        xSemaphoreGive(i2cMutex);
    }

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        Serial.println("✓ GPS Serial2 initialized");
        Serial.println("✓ GUVA-S12SD UV sensor on analog pin");
        xSemaphoreGive(serialMutex);
    }

    // Calibrate altitude (assume current location is reference point)
    calibrateAltitude();

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        Serial.println("✓ Sensor initialization complete");
        xSemaphoreGive(serialMutex);
    }
}

void scanI2CBus()
{
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        Serial.println("Scanning I2C bus...");
        xSemaphoreGive(serialMutex);
    }

    byte error, address;
    int nDevices = 0;
    for (address = 1; address < 127; address++)
    {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if (error == 0)
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.print("I2C device found at address 0x");
                if (address < 16)
                    Serial.print("0");
                Serial.println(address, HEX);
                xSemaphoreGive(serialMutex);
            }
            nDevices++;
        }
    }

    if (nDevices == 0)
    {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Serial.println("No I2C devices found");
            xSemaphoreGive(serialMutex);
        }
    }
}

void calibrateAltitude()
{
    if (!sensorsInitialized)
    {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Serial.println("⚠ BME280 not available - altitude will use default sea level pressure");
            xSemaphoreGive(serialMutex);
        }
        return;
    }

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        Serial.println("Calibrating altitude reference...");
        xSemaphoreGive(serialMutex);
    }

    // Wait for sensor to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Take multiple pressure readings for calibration
    float pressureSum = 0;
    int validReadings = 0;

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5000)) == pdTRUE)
    {
        for (int i = 0; i < 30; i++)
        {
            // Force a reading and wait
            bme280.takeForcedMeasurement();
            vTaskDelay(pdMS_TO_TICKS(100));

            float pressure = bme280.readPressure() / 100.0; // Convert Pa to hPa
            if (pressure > 800 && pressure < 1200)          // Reasonable pressure range
            {
                pressureSum += pressure;
                validReadings++;
                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    Serial.print(".");
                    xSemaphoreGive(serialMutex);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(i2cMutex);
    }

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        Serial.println();
        xSemaphoreGive(serialMutex);
    }

    if (validReadings > 20)
    {
        seaLevelPressure = pressureSum / validReadings;
        altitudeCalibrated = true;

        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Serial.print("✓ Altitude calibrated - Reference pressure: ");
            Serial.print(seaLevelPressure);
            Serial.println(" hPa (current location = 0m)");
            xSemaphoreGive(serialMutex);
        }
    }
    else
    {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Serial.println("✗ Altitude calibration failed - using standard sea level pressure");
            xSemaphoreGive(serialMutex);
        }
    }
}

void readEnvironmentalSensors()
{
    // Create local copy to minimize mutex lock time
    TelemetryPacket localTelemetry;

    // Read BME280 (temperature and pressure)
    if (sensorsInitialized)
    {
        // Force a measurement for consistent timing
        bme280.takeForcedMeasurement();

        float temp = bme280.readTemperature();
        float currentPressure = bme280.readPressure() / 100.0; // Convert Pa to hPa

        localTelemetry.temperature = (int16_t)(temp * 100);
        localTelemetry.pressure = (uint16_t)(currentPressure * 10);

        // Validate pressure reading
        if (currentPressure > 800 && currentPressure < 1200)
        {
            // Add to pressure buffer for simple smoothing
            pressureReadings[pressureIndex] = currentPressure;
            pressureIndex = (pressureIndex + 1) % PRESSURE_BUFFER_SIZE;
            if (pressureCount < PRESSURE_BUFFER_SIZE)
                pressureCount++;

            // Calculate simple moving average
            float pressureSum = 0;
            for (int i = 0; i < pressureCount; i++)
            {
                pressureSum += pressureReadings[i];
            }
            float smoothedPressure = pressureSum / pressureCount;

            // Apply exponential smoothing for additional stability
            exponentialPressure = (PRESSURE_SMOOTHING_ALPHA * smoothedPressure) +
                                  ((1.0 - PRESSURE_SMOOTHING_ALPHA) * exponentialPressure);

            // Temperature compensation for BME280
            float currentTemperature = telemetryData.temperature / 100.0;                         // Convert from scaled integer (x100) to degrees C
            float tempCompensation = 1.0 + (currentTemperature - referenceTemperature) * 0.00366; // ~0.366% per °C
            float compensatedPressure = exponentialPressure * tempCompensation;

            // Calculate altitude using temperature-compensated pressure
            float altitude;
            if (altitudeCalibrated)
            {
                // Use calibrated reference for relative altitude with temperature compensation
                altitude = 44330.0 * (1.0 - pow(compensatedPressure / seaLevelPressure, 0.1903));
            }
            else
            {
                // Use standard sea level pressure with temperature compensation
                altitude = 44330.0 * (1.0 - pow(compensatedPressure / 1013.25, 0.1903));
            }

            // Apply altitude offset for zero-reference
            altitude -= altitudeOffset;

            // Debug output for pressure and altitude calculation (less frequent)
            static unsigned long lastDebug = 0;
            static int debugCounter = 0;
            if (millis() - lastDebug > 5000) // Debug every 5 seconds
            {
                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    Serial.print("DEBUG - P: ");
                    Serial.print(currentPressure, 2);
                    Serial.print("/");
                    Serial.print(exponentialPressure, 2);
                    Serial.print(" hPa, Ref: ");
                    Serial.print(seaLevelPressure, 2);
                    Serial.print(" hPa, Alt: ");
                    Serial.print(altitude, 2);
                    Serial.print(" m, Temp: ");
                    Serial.print(telemetryData.temperature, 1);
                    Serial.print("°C, YawBias: ");
                    Serial.print(adaptiveGyroZBias, 3);
                    Serial.print(", Stationary: ");
                    Serial.println(isStationary ? "YES" : "NO");
                    xSemaphoreGive(serialMutex);
                }
                lastDebug = millis();
                debugCounter++;
            }

            // Simple range limiting
            if (altitude < -500)
                altitude = -500;
            if (altitude > 5000)
                altitude = 5000;

            // Store altitude in centimeters for 2 decimal precision
            localTelemetry.altitude = (int16_t)(altitude * 100); // Convert meters to centimeters
        }
        else
        {
            // Invalid pressure reading - keep current altitude
            if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                localTelemetry.altitude = telemetryData.altitude;
                xSemaphoreGive(telemetryMutex);
            }
        }
    }
    else
    {
        // Fallback to simulated data
        static float temp = 25.0;
        temp += (random(-10, 11) / 100.0);
        localTelemetry.temperature = (int16_t)(temp * 100);

        static float pressure = 1013.2;
        pressure += (random(-5, 6) / 10.0);
        localTelemetry.pressure = (uint16_t)(pressure * 10);

        localTelemetry.altitude = 10000 + random(-2000, 2001); // Simulated altitude in cm
    }

    // Read AHT21 (humidity and temperature)
    sensors_event_t humidity, temp;
    if (aht.getEvent(&humidity, &temp))
    {
        localTelemetry.humidity = (uint8_t)humidity.relative_humidity;
        // Use AHT21 temperature if BME280 failed
        if (!sensorsInitialized)
        {
            localTelemetry.temperature = (int16_t)(temp.temperature * 100);
        }
    }
    else
    {
        // Fallback to simulated humidity
        localTelemetry.humidity = 60 + random(-10, 11);
    }

    // Read real battery voltage
    int batteryRaw = analogRead(BATTERY_PIN);
    if (batteryRaw < 100)
    {
        // Simulate battery if not connected
        localTelemetry.battery = 3700 + random(-100, 101);
    }
    else
    {
        localTelemetry.battery = map(batteryRaw, 0, 4095, 0, 5000);
    }

    // Read real light sensor (BH1750)
    if (bh1750Ready)
    {
        float lux = lightMeter.readLightLevel();
        if (lux >= 0)
        {
            localTelemetry.lux = (uint16_t)lux;
        }
        else
        {
            localTelemetry.lux = 500 + random(-100, 101); // Fallback simulated
        }
    }
    else
    {
        localTelemetry.lux = 500 + random(-100, 101); // Simulated if sensor not ready
    }

    // Read real UV sensor (GUVA-S12SD)
    int uvRaw = analogRead(GUVA_PIN);
    // Convert to UV index (calibration may need adjustment based on your setup)
    float uvVoltage = (uvRaw / 4095.0) * 3.3;
    float uvIndex = uvVoltage / 0.1; // Approximate conversion for GUVA-S12SD
    if (uvIndex < 0)
        uvIndex = 0; // Clamp to positive values
    if (uvIndex > 15)
        uvIndex = 15; // Reasonable UV index maximum
    localTelemetry.uvIndex = (uint16_t)(uvIndex * 100);

    // Read real air quality sensor (ENS160)
    if (ens160Ready && ens160.available())
    {
        ens160.measure();
        uint16_t eco2 = ens160.geteCO2();
        uint16_t tvoc = ens160.getTVOC();

        // Validate readings (ENS160 sometimes gives invalid values during warm-up)
        if (eco2 > 400 && eco2 < 5000) // Reasonable CO2 range
        {
            localTelemetry.eCO2 = eco2;
        }
        else
        {
            localTelemetry.eCO2 = 400 + random(-50, 51); // Fallback
        }

        if (tvoc < 1000) // Reasonable TVOC range
        {
            localTelemetry.TVOC = tvoc;
        }
        else
        {
            localTelemetry.TVOC = 50 + random(-20, 21); // Fallback
        }
    }
    else
    {
        // Fallback to simulated values
        localTelemetry.eCO2 = 400 + random(-50, 51);
        localTelemetry.TVOC = 50 + random(-20, 21);
    }

    // Update status
    localTelemetry.status = 0x01; // All systems OK
    if (!sensorsInitialized)
        localTelemetry.status |= 0x02; // Sensor warning

    // Get GPS data from shared telemetry (GPS is updated in its own function)
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        localTelemetry.latitude = telemetryData.latitude;
        localTelemetry.longitude = telemetryData.longitude;
        localTelemetry.satellites = telemetryData.satellites;

        // Update all telemetry data atomically
        telemetryData = localTelemetry;
        xSemaphoreGive(telemetryMutex);
    }
}

void updateGPS()
{
    // Read GPS data from Serial2
    while (Serial2.available() > 0)
    {
        if (gps.encode(Serial2.read()))
        {
            if (gps.location.isValid())
            {
                // Update GPS data in shared telemetry
                if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    telemetryData.latitude = (int16_t)(gps.location.lat() * 100);
                    telemetryData.longitude = (int16_t)(gps.location.lng() * 100);
                    telemetryData.satellites = gps.satellites.value();
                    xSemaphoreGive(telemetryMutex);
                }
            }
        }
    }

    // If no GPS fix, use default values
    if (!gps.location.isValid())
    {
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            telemetryData.latitude = 0;
            telemetryData.longitude = 0;
            telemetryData.satellites = 0;
            xSemaphoreGive(telemetryMutex);
        }
    }
}

void handleControlData()
{
    if (radio.available())
    {
        uint8_t len = radio.getDynamicPayloadSize();

        if (len == sizeof(ControlPacket))
        {
            ControlPacket localControl;
            radio.read(&localControl, sizeof(localControl));

            // Update shared control data (for telemetry display)
            if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                lastControlReceived = millis();
                packetsReceived++;
                xSemaphoreGive(telemetryMutex);
            }

            // Update control data for motor task (separate mutex for higher priority)
            if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(5)) == pdTRUE)
            {
                receivedControl = localControl;
                lastValidControl = millis(); // Update motor control timeout
                xSemaphoreGive(controlMutex);
            }

            // Print received control data with real sensor info (thread-safe)
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                Serial.print("Control #");
                Serial.print(packetsReceived);
                Serial.print(" - T:");
                Serial.print(localControl.throttle);
                Serial.print(" R:");
                Serial.print(localControl.roll);
                Serial.print(" P:");
                Serial.print(localControl.pitch);
                Serial.print(" Y:");
                Serial.print(localControl.yaw);

                // Show button states when pressed
                if (localControl.joy1_btn == 0)
                    Serial.print(" [JOY1-BTN]");
                if (localControl.joy2_btn == 0)
                    Serial.print(" [JOY2-BTN]");

                // Show toggle switch states
                if (localControl.toggle1 == 1)
                    Serial.print(" [SW1-ARM]");
                if (localControl.toggle2 == 1)
                    Serial.print(" [SW2-ESTOP]");

                // Get current telemetry for display
                TelemetryPacket currentTelemetry;
                if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    currentTelemetry = telemetryData;
                    xSemaphoreGive(telemetryMutex);
                }

                Serial.print(" | Sensors - Temp:");
                Serial.print(currentTelemetry.temperature / 100.0);
                Serial.print("°C Hum:");
                Serial.print(currentTelemetry.humidity);
                Serial.print("% Press:");
                Serial.print(currentTelemetry.pressure / 10.0);
                Serial.print("hPa Alt:");
                Serial.print(currentTelemetry.altitude / 100.0); // Display altitude in meters with 2 decimals
                Serial.print("m GPS:");
                Serial.print(currentTelemetry.satellites);
                Serial.print(" sats Lux:");
                Serial.print(currentTelemetry.lux);
                Serial.print("lx UV:");
                Serial.print(currentTelemetry.uvIndex / 100.0);
                Serial.print(" CO2:");
                Serial.print(currentTelemetry.eCO2);
                Serial.print("ppm TVOC:");
                Serial.print(currentTelemetry.TVOC);
                Serial.println("ppb");

                xSemaphoreGive(serialMutex);
            }

            // Load telemetry data as ACK payload for NEXT packet
            TelemetryPacket ackTelemetry;
            if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                ackTelemetry = telemetryData;
                xSemaphoreGive(telemetryMutex);
            }
            radio.writeAckPayload(1, &ackTelemetry, sizeof(ackTelemetry));
        }
        else
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                Serial.print("Wrong packet size: ");
                Serial.println(len);
                xSemaphoreGive(serialMutex);
            }
        }
    }
}

void printStatus()
{
    unsigned long uptime = millis() / 1000;
    unsigned long lastControl;
    int packets;
    TelemetryPacket currentTelemetry;

    // Get shared data safely
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        lastControl = lastControlReceived;
        packets = packetsReceived;
        currentTelemetry = telemetryData;
        xSemaphoreGive(telemetryMutex);
    }

    bool controlActive = (millis() - lastControl) < 3000;

    Serial.print("Drone status [FreeRTOS+PID] - Running ");
    Serial.print(uptime);
    Serial.print("s, Control packets: ");
    Serial.print(packets);
    Serial.print(", Active: ");
    Serial.print(controlActive ? "YES" : "NO");

    // Flight mode and motor status
    Serial.print(", Mode: ");
    switch (currentFlightMode)
    {
    case FLIGHT_MODE_DISARMED:
        Serial.print("DISARMED");
        break;
    case FLIGHT_MODE_MANUAL:
        Serial.print("MANUAL");
        break;
    case FLIGHT_MODE_STABILIZE:
        Serial.print("STABILIZE");
        break;
    case FLIGHT_MODE_ALTITUDE_HOLD:
        Serial.print("ALT_HOLD");
        break;
    case FLIGHT_MODE_POSITION_HOLD:
        Serial.print("POS_HOLD");
        break;
    default:
        Serial.print("UNKNOWN");
        break;
    }

    Serial.print(", Motors: ");
    Serial.print(motorsArmed ? "ARMED" : "DISARMED");
    
    // Show flight mode
    Serial.print(stabilizedMode ? " [STABILIZED]" : " [MANUAL]");

    // IMU Status
    Serial.print(", IMU: ");
    if (imuInitialized && imuCalibration.calibrated)
    {
        if (xSemaphoreTake(imuMutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            Serial.print("OK [R:");
            Serial.print(imuData.roll, 1);
            Serial.print("° P:");
            Serial.print(imuData.pitch, 1);
            Serial.print("° Y:");
            Serial.print(imuData.yaw, 1);
            Serial.print("°]");
            xSemaphoreGive(imuMutex);
        }
    }
    else if (imuInitialized && !imuCalibration.calibrated)
    {
        Serial.print("CALIBRATING(");
        Serial.print(imuCalibration.calibrationSamples);
        Serial.print("/500)");
    }
    else
    {
        Serial.print("FAILED");
    }

    // PID Status (simplified - just enabled/disabled)
    Serial.print(", PID: ");
    Serial.print(pidEnabled ? "ENABLED" : "DISABLED");

    // Environmental sensors
    Serial.print(", Sensors: Temp:");
    Serial.print(currentTelemetry.temperature / 100.0);
    Serial.print("°C Hum:");
    Serial.print(currentTelemetry.humidity);
    Serial.print("% Press:");
    Serial.print(currentTelemetry.pressure / 10.0);
    Serial.print("hPa Alt:");
    Serial.print(currentTelemetry.altitude / 100.0); // Display altitude in meters with 2 decimals
    Serial.print("m GPS:");
    Serial.print(currentTelemetry.satellites);
    Serial.print(" sats Batt:");
    Serial.print(currentTelemetry.battery);
    Serial.print("mV Lux:");
    Serial.print(currentTelemetry.lux);
    Serial.print("lx UV:");
    Serial.print(currentTelemetry.uvIndex / 100.0);
    Serial.print(" CO2:");
    Serial.print(currentTelemetry.eCO2);
    Serial.print("ppm TVOC:");
    Serial.print(currentTelemetry.TVOC);
    Serial.println("ppb");
}

// Motor Control Task
void motorTask(void *parameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t motorFrequency = pdMS_TO_TICKS(20); // 50Hz motor update (20ms)

    for (;;)
    {
        // Check for control timeout (safety feature)
        unsigned long currentTime = millis();
        bool controlValid = (currentTime - lastValidControl) < CONTROL_TIMEOUT_MS;

        if (!controlValid)
        {
            // Safety: Stop all motors if no recent control
            for (int i = 0; i < 4; i++)
            {
                motorSpeeds[i] = ESC_ARM_PULSE;
            }
            motorsArmed = false;
        }
        else if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            // Process control inputs and calculate motor speeds
            calculateMotorSpeeds();
            xSemaphoreGive(controlMutex);
        }

        // Write motor speeds to ESCs
        esc1.writeMicroseconds(motorSpeeds[0]); // Front Right (CCW)
        esc2.writeMicroseconds(motorSpeeds[1]); // Back Right (CW)
        esc3.writeMicroseconds(motorSpeeds[2]); // Front Left (CW)
        esc4.writeMicroseconds(motorSpeeds[3]); // Back Left (CCW)

        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, motorFrequency);
    }
}

// Calculate Motor Speeds from Control Inputs with PID Integration
void calculateMotorSpeeds()
{
    // Check if motors should be armed (toggle switch 1 controls arming)
    if (receivedControl.toggle1 == 1)
    {
        bool wasDisarmed = !motorsArmed;
        motorsArmed = true;

        // When first armed, check toggle2 for flight mode selection
        if (wasDisarmed && currentFlightMode == FLIGHT_MODE_DISARMED)
        {
            // Toggle 2 controls flight mode: ON = Stabilized, OFF = Manual
            if (receivedControl.toggle2 == 1)
            {
                currentFlightMode = FLIGHT_MODE_STABILIZE;
                pidEnabled = true; // Enable PID for stabilized mode
                stabilizedMode = true; // Update status variable
                
                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    Serial.println("🔓 MOTORS ARMED - STABILIZED MODE ACTIVE (PID ON)");
                    xSemaphoreGive(serialMutex);
                }
            }
            else
            {
                currentFlightMode = FLIGHT_MODE_MANUAL;
                pidEnabled = false; // Disable PID for manual mode
                stabilizedMode = false; // Update status variable
                
                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    Serial.println("🔓 MOTORS ARMED - MANUAL MODE ACTIVE (PID OFF)");
                    xSemaphoreGive(serialMutex);
                }
            }
        }
        else if (motorsArmed)
        {
            // Motors already armed - check for flight mode changes during flight
            static bool lastModeToggle = false;
            bool currentModeToggle = (receivedControl.toggle2 == 1);
            
            if (currentModeToggle != lastModeToggle)
            {
                if (currentModeToggle)
                {
                    // Switch to Stabilized mode
                    currentFlightMode = FLIGHT_MODE_STABILIZE;
                    pidEnabled = true;
                    stabilizedMode = true; // Update status variable
                    
                    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        Serial.println("🛡️ FLIGHT MODE: STABILIZED (PID ON)");
                        xSemaphoreGive(serialMutex);
                    }
                }
                else
                {
                    // Switch to Manual mode
                    currentFlightMode = FLIGHT_MODE_MANUAL;
                    pidEnabled = false;
                    stabilizedMode = false; // Update status variable
                    
                    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        Serial.println("🎯 FLIGHT MODE: MANUAL (PID OFF)");
                        xSemaphoreGive(serialMutex);
                    }
                }
                lastModeToggle = currentModeToggle;
            }
        }
    }
    else
    {
        // Motors being disarmed - trigger pressure re-calibration
        static bool wasArmed = false;
        if (wasArmed && motorsArmed) // Transition from armed to disarmed
        {
            // Re-calibrate pressure reference and reset altitude offset
            if (pressureCount >= PRESSURE_BUFFER_SIZE / 2) // Ensure we have enough samples
            {
                seaLevelPressure = exponentialPressure;
                altitudeOffset = 44330.0 * (1.0 - pow(exponentialPressure / seaLevelPressure, 0.1903));
                altitudeCalibrated = true;
                lastAltitudeCalibration = millis();

                if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    Serial.printf("Altitude re-calibrated on disarm - Ref P: %.2f hPa, Offset: %.2f m\n",
                                  seaLevelPressure, altitudeOffset);
                    xSemaphoreGive(serialMutex);
                }
            }

            // Reset yaw angle to prevent accumulation
            imuData.yaw = 0.0;
            lastYawReset = millis();
        }
        wasArmed = motorsArmed;
        motorsArmed = false;
        stabilizedMode = false; // Reset flight mode status
        currentFlightMode = FLIGHT_MODE_DISARMED;
        pidEnabled = false;
    }

    if (!motorsArmed)
    {
        // Motors disarmed - all ESCs to minimum
        for (int i = 0; i < 4; i++)
        {
            motorSpeeds[i] = ESC_ARM_PULSE;
        }
        return;
    }

    // Convert throttle input with full 2000μs range support and intelligent headroom management
    // Virtual Throttle Mode: Remote sends -3000 to +3000 where:
    // -3000 = 0% power (minimum/idle), +3000 = 100% power (maximum = 2000μs when no control applied)
    // When control inputs are active, base throttle is intelligently reduced to maintain differential control

    int throttleInput;
    int baseThrottle;

    if (receivedControl.throttle <= -3000)
    {
        // At or below minimum - motors idle
        throttleInput = 0;
        baseThrottle = ESC_ARM_PULSE;
    }
    else
    {
        // Calculate current control input magnitudes
        int currentRollCorrection = 0;
        int currentPitchCorrection = 0; 
        int currentYawCorrection = 0;
        
        if (currentFlightMode == FLIGHT_MODE_MANUAL)
        {
            // Manual mode - use actual stick inputs
            currentRollCorrection = abs(map(receivedControl.roll, -3000, 3000, -200, 200));
            currentPitchCorrection = abs(map(receivedControl.pitch, -3000, 3000, -200, 200));
            currentYawCorrection = abs(map(receivedControl.yaw, -3000, 3000, -150, 150));
        }
        else if (currentFlightMode >= FLIGHT_MODE_STABILIZE)
        {
            // Stabilized mode - use actual PID outputs (thread-safe read)
            if (xSemaphoreTake(pidMutex, pdMS_TO_TICKS(2)) == pdTRUE)
            {
                currentRollCorrection = abs((int)pidRollOutput);
                currentPitchCorrection = abs((int)pidPitchOutput);
                currentYawCorrection = abs((int)pidYawOutput);
                xSemaphoreGive(pidMutex);
            }
        }
        
        // Calculate required headroom based on ACTUAL control inputs (not theoretical maximum)
        // This allows full 2000μs when no control is applied, but intelligently reserves space when needed
        int requiredHeadroom = currentRollCorrection + currentPitchCorrection + currentYawCorrection;
        
        // Apply a small safety margin (10% of required headroom) to prevent brief overshoots
        requiredHeadroom = (int)(requiredHeadroom * 1.1);
        
        // Calculate maximum safe base throttle with dynamic headroom
        int maxSafeBaseThrottle = ESC_MAX_PULSE - requiredHeadroom;
        maxSafeBaseThrottle = constrain(maxSafeBaseThrottle, ESC_ARM_PULSE, ESC_MAX_PULSE);
        
        // Map full throttle input range to available base throttle range
        int availableThrottleRange = maxSafeBaseThrottle - ESC_ARM_PULSE;
        throttleInput = map(receivedControl.throttle, -3000, 3000, 0, availableThrottleRange);
        baseThrottle = ESC_ARM_PULSE + constrain(throttleInput, 0, availableThrottleRange);
    }

    // Get PID outputs (thread-safe)
    float rollCorrection = 0.0, pitchCorrection = 0.0, yawCorrection = 0.0, altitudeCorrection = 0.0;
    if (xSemaphoreTake(pidMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        rollCorrection = pidRollOutput;
        pitchCorrection = pidPitchOutput;
        yawCorrection = pidYawOutput;
        altitudeCorrection = pidAltitudeOutput;
        xSemaphoreGive(pidMutex);
    }

    // Motor mixing based on flight mode
    if (currentFlightMode == FLIGHT_MODE_MANUAL)
    {
        // Manual mode - direct stick control (no PID)
        int rollInput = map(receivedControl.roll, -3000, 3000, -200, 200);
        int pitchInput = map(receivedControl.pitch, -3000, 3000, -200, 200);
        int yawInput = map(receivedControl.yaw, -3000, 3000, -150, 150);

        // Standard X-configuration mixing
        motorSpeeds[0] = baseThrottle + rollInput + pitchInput - yawInput; // Front Right (CCW)
        motorSpeeds[1] = baseThrottle + rollInput - pitchInput + yawInput; // Back Right (CW)
        motorSpeeds[2] = baseThrottle - rollInput + pitchInput + yawInput; // Front Left (CW)
        motorSpeeds[3] = baseThrottle - rollInput - pitchInput - yawInput; // Back Left (CCW)
    }
    else if (currentFlightMode >= FLIGHT_MODE_STABILIZE)
    {
        // Stabilized mode - use PID corrections
        // Apply altitude correction to base throttle if in altitude hold mode
        float adjustedBaseThrottle = baseThrottle;
        if (currentFlightMode >= FLIGHT_MODE_ALTITUDE_HOLD)
        {
            adjustedBaseThrottle += altitudeCorrection;
        }

        // Apply PID corrections to motor mixing
        // Standard X-configuration with PID corrections
        motorSpeeds[0] = adjustedBaseThrottle + rollCorrection + pitchCorrection - yawCorrection; // Front Right (CCW)
        motorSpeeds[1] = adjustedBaseThrottle + rollCorrection - pitchCorrection + yawCorrection; // Back Right (CW)
        motorSpeeds[2] = adjustedBaseThrottle - rollCorrection + pitchCorrection + yawCorrection; // Front Left (CW)
        motorSpeeds[3] = adjustedBaseThrottle - rollCorrection - pitchCorrection - yawCorrection; // Back Left (CCW)
    }

    // Enhanced motor speed limiting with intelligent differential control
    // First constrain to basic range
    for (int i = 0; i < 4; i++)
    {
        motorSpeeds[i] = constrain(motorSpeeds[i], ESC_ARM_PULSE, ESC_MAX_PULSE);
    }
    
    // Advanced safety: If any motor exceeds limits, use differential motor compensation
    // Instead of scaling all motors down, increase opposite motors and decrease base throttle
    int maxMotorSpeed = max(max(motorSpeeds[0], motorSpeeds[1]), max(motorSpeeds[2], motorSpeeds[3]));
    int minMotorSpeed = min(min(motorSpeeds[0], motorSpeeds[1]), min(motorSpeeds[2], motorSpeeds[3]));
    
    if (maxMotorSpeed > ESC_MAX_PULSE)
    {
        // Calculate excess that needs to be compensated
        int excess = maxMotorSpeed - ESC_MAX_PULSE;
        
        // Differential compensation: reduce all motors by the excess amount
        // This maintains the relative differences while keeping all motors within limits
        for (int i = 0; i < 4; i++)
        {
            motorSpeeds[i] -= excess;
            motorSpeeds[i] = constrain(motorSpeeds[i], ESC_ARM_PULSE, ESC_MAX_PULSE);
        }
        
        // The control authority is now maintained through differential speeds
        // rather than absolute motor speeds - this is how professional flight controllers work
    }
    
    // Additional safety check - if any motor is still below minimum, bring all up proportionally
    minMotorSpeed = min(min(motorSpeeds[0], motorSpeeds[1]), min(motorSpeeds[2], motorSpeeds[3]));
    if (minMotorSpeed < ESC_ARM_PULSE)
    {
        int deficit = ESC_ARM_PULSE - minMotorSpeed;
        for (int i = 0; i < 4; i++)
        {
            motorSpeeds[i] += deficit;
            motorSpeeds[i] = constrain(motorSpeeds[i], ESC_ARM_PULSE, ESC_MAX_PULSE);
        }
    }

    // Enhanced debug output with intelligent throttle management info (less frequent to avoid overwhelming serial)
    static unsigned long lastMotorDebug = 0;
    if (millis() - lastMotorDebug > 2000) // Every 2 seconds
    {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            // Calculate current throttle percentage and control activity
            int maxCurrentMotor = max(max(motorSpeeds[0], motorSpeeds[1]), max(motorSpeeds[2], motorSpeeds[3]));
            int minCurrentMotor = min(min(motorSpeeds[0], motorSpeeds[1]), min(motorSpeeds[2], motorSpeeds[3]));
            int throttlePercent = map(baseThrottle, ESC_ARM_PULSE, ESC_MAX_PULSE, 0, 100);
            int controlActivity = maxCurrentMotor - minCurrentMotor; // Shows how much control authority is being used
            
            Serial.print("🚁 Motors: Armed:");
            Serial.print(motorsArmed ? "YES" : "NO");
            Serial.print(" | Throttle: Raw:");
            Serial.print(receivedControl.throttle);
            Serial.print(" Base:");
            Serial.print(baseThrottle);
            Serial.print("μs (");
            Serial.print(throttlePercent);
            Serial.print("%) | Motor Range: ");
            Serial.print(minCurrentMotor);
            Serial.print("-");
            Serial.print(maxCurrentMotor);
            Serial.print("μs | Control Activity:");
            Serial.print(controlActivity);
            Serial.print("μs | Speeds:[");
            Serial.print(motorSpeeds[0]);
            Serial.print(",");
            Serial.print(motorSpeeds[1]);
            Serial.print(",");
            Serial.print(motorSpeeds[2]);
            Serial.print(",");
            Serial.print(motorSpeeds[3]);
            Serial.print("] | Mode:");
            switch (currentFlightMode)
            {
            case FLIGHT_MODE_DISARMED:
                Serial.print("DISARMED");
                break;
            case FLIGHT_MODE_MANUAL:
                Serial.print("MANUAL");
                break;
            case FLIGHT_MODE_STABILIZE:
                Serial.print("STABILIZE");
                break;
            case FLIGHT_MODE_ALTITUDE_HOLD:
                Serial.print("ALT_HOLD");
                break;
            default:
                Serial.print("UNKNOWN");
                break;
            }

            Serial.print(" [Armed:");
            Serial.print(motorsArmed ? "YES" : "NO");
            Serial.print("] T:");
            Serial.print(throttleInput);

            if (currentFlightMode >= FLIGHT_MODE_STABILIZE && imuData.dataValid)
            {
                Serial.print(" | IMU R:");
                Serial.print(imuData.roll, 1);
                Serial.print(" P:");
                Serial.print(imuData.pitch, 1);
                Serial.print(" Y:");
                Serial.print(imuData.yaw, 1);
            }

            Serial.print(" | Motors:");
            Serial.print(motorSpeeds[0]);
            Serial.print(",");
            Serial.print(motorSpeeds[1]);
            Serial.print(",");
            Serial.print(motorSpeeds[2]);
            Serial.print(",");
            Serial.println(motorSpeeds[3]);
            xSemaphoreGive(serialMutex);
        }
        lastMotorDebug = millis();
    }
}

// ================================
// PID AND IMU CONTROL FUNCTIONS
// ================================

// Initialize IMU and PID Controllers
void initializeIMUAndPID()
{
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        Serial.println("Initializing MPU6050 IMU and PID controllers...");
        xSemaphoreGive(serialMutex);
    }

    // Initialize MPU6050
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE)
    {
        if (mpu6050.begin(MPU6050_ADDRESS))
        {
            // Configure MPU6050 for optimal performance
            mpu6050.setAccelerometerRange(MPU6050_RANGE_8_G); // ±8g range
            mpu6050.setGyroRange(MPU6050_RANGE_500_DEG);      // ±500°/s range
            mpu6050.setFilterBandwidth(MPU6050_BAND_21_HZ);   // 21Hz low-pass filter

            imuInitialized = true;

            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✓ MPU6050 initialized successfully");
                xSemaphoreGive(serialMutex);
            }
        }
        else
        {
            if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                Serial.println("✗ MPU6050 initialization failed - PID disabled");
                xSemaphoreGive(serialMutex);
            }
            imuInitialized = false;
        }
        xSemaphoreGive(i2cMutex);
    }

    // Initialize PID controllers with advanced features
    rollPID.initialize(pidConfig.roll_kp, pidConfig.roll_ki, pidConfig.roll_kd,
                       -PID_ROLL_PITCH_MAX, PID_ROLL_PITCH_MAX);
    rollPID.setDerivativeFilter(pidConfig.roll_filter);
    rollPID.setSetpointSmoothing(pidConfig.roll_smooth);
    rollPID.setIntegralLimits(0.2, 0.2); // 20% of output range for integral

    pitchPID.initialize(pidConfig.pitch_kp, pidConfig.pitch_ki, pidConfig.pitch_kd,
                        -PID_ROLL_PITCH_MAX, PID_ROLL_PITCH_MAX);
    pitchPID.setDerivativeFilter(pidConfig.pitch_filter);
    pitchPID.setSetpointSmoothing(pidConfig.pitch_smooth);
    pitchPID.setIntegralLimits(0.2, 0.2); // 20% of output range for integral

    yawPID.initialize(pidConfig.yaw_kp, pidConfig.yaw_ki, pidConfig.yaw_kd,
                      -PID_YAW_MAX, PID_YAW_MAX);
    yawPID.setDerivativeFilter(pidConfig.yaw_filter);
    yawPID.setSetpointSmoothing(pidConfig.yaw_smooth);
    yawPID.setIntegralLimits(0.15, 0.15); // 15% of output range for yaw integral

    altitudePID.initialize(pidConfig.alt_kp, pidConfig.alt_ki, pidConfig.alt_kd,
                           -PID_ALTITUDE_MAX, PID_ALTITUDE_MAX);
    altitudePID.setDerivativeFilter(pidConfig.alt_filter);
    altitudePID.setSetpointSmoothing(pidConfig.alt_smooth);
    altitudePID.setIntegralLimits(0.3, 0.3); // 30% of output range for altitude integral    // Initialize IMU data structure
    imuData.dataValid = false;
    imuData.timestamp = 0;

    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        Serial.println("✓ PID controllers initialized");
        if (imuInitialized)
        {
            Serial.println("✓ Starting IMU calibration...");
        }
        xSemaphoreGive(serialMutex);
    }
}

// IMU Task - High frequency IMU reading (100Hz)
void imuTask(void *parameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t imuFrequency = pdMS_TO_TICKS(10); // 100Hz IMU reading

    // Calibration variables
    const int CALIBRATION_SAMPLES = 500; // 5 seconds of calibration at 100Hz
    float gyroXSum = 0, gyroYSum = 0, gyroZSum = 0;
    float rollSum = 0, pitchSum = 0;

    for (;;)
    {
        if (imuInitialized && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE)
        {
            sensors_event_t accel, gyro, temp;
            bool readSuccess = mpu6050.getEvent(&accel, &gyro, &temp);
            xSemaphoreGive(i2cMutex);

            if (readSuccess && xSemaphoreTake(imuMutex, pdMS_TO_TICKS(5)) == pdTRUE)
            {
                // Raw sensor data
                imuData.accelX = accel.acceleration.x;
                imuData.accelY = accel.acceleration.y;
                imuData.accelZ = accel.acceleration.z;
                imuData.gyroX = gyro.gyro.x * 180.0 / PI; // Convert to deg/s
                imuData.gyroY = gyro.gyro.y * 180.0 / PI;
                imuData.gyroZ = gyro.gyro.z * 180.0 / PI;
                imuData.temperature = temp.temperature;

                // Calculate roll and pitch from accelerometer
                float roll_acc = atan2(imuData.accelY, imuData.accelZ) * 180.0 / PI;
                float pitch_acc = atan2(-imuData.accelX, sqrt(imuData.accelY * imuData.accelY + imuData.accelZ * imuData.accelZ)) * 180.0 / PI;

                // Calibration phase
                if (!imuCalibration.calibrated)
                {
                    if (imuCalibration.calibrationSamples < CALIBRATION_SAMPLES)
                    {
                        gyroXSum += imuData.gyroX;
                        gyroYSum += imuData.gyroY;
                        gyroZSum += imuData.gyroZ;
                        rollSum += roll_acc;
                        pitchSum += pitch_acc;
                        imuCalibration.calibrationSamples++;
                    }
                    else
                    {
                        // Calculate offsets
                        imuCalibration.gyroXOffset = gyroXSum / CALIBRATION_SAMPLES;
                        imuCalibration.gyroYOffset = gyroYSum / CALIBRATION_SAMPLES;
                        imuCalibration.gyroZOffset = gyroZSum / CALIBRATION_SAMPLES;
                        imuCalibration.rollOffset = rollSum / CALIBRATION_SAMPLES;
                        imuCalibration.pitchOffset = pitchSum / CALIBRATION_SAMPLES;
                        imuCalibration.calibrated = true;

                        // Initialize complementary filter with calibrated values
                        imuData.roll = 0.0;  // Start level
                        imuData.pitch = 0.0; // Start level
                        imuData.yaw = 0.0;   // Start at 0

                        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            Serial.println("✓ IMU calibration complete!");
                            Serial.printf("Gyro offsets - X: %.2f, Y: %.2f, Z: %.2f deg/s\n",
                                          imuCalibration.gyroXOffset, imuCalibration.gyroYOffset, imuCalibration.gyroZOffset);
                            Serial.printf("Level offsets - Roll: %.2f, Pitch: %.2f degrees\n",
                                          imuCalibration.rollOffset, imuCalibration.pitchOffset);
                            xSemaphoreGive(serialMutex);
                        }
                    }
                }
                else
                {
                    // Apply calibration offsets
                    float gyroXCal = imuData.gyroX - imuCalibration.gyroXOffset;
                    float gyroYCal = imuData.gyroY - imuCalibration.gyroYOffset;
                    float gyroZCal = imuData.gyroZ - imuCalibration.gyroZOffset;
                    float rollAccCal = roll_acc - imuCalibration.rollOffset;
                    float pitchAccCal = pitch_acc - imuCalibration.pitchOffset;

                    // Enhanced yaw drift correction
                    // Check if drone is stationary (low angular rates on all axes)
                    float totalAngularRate = abs(gyroXCal) + abs(gyroYCal) + abs(gyroZCal);
                    if (totalAngularRate < 1.0 && !motorsArmed) // Less than 1 deg/s on all axes and disarmed
                    {
                        if (!isStationary)
                        {
                            isStationary = true;
                            stationaryStartTime = millis();
                        }
                        else if (millis() - stationaryStartTime > STATIONARY_THRESHOLD_TIME)
                        {
                            // Accumulate bias samples when drone is definitely stationary
                            if (gyroBiasSampleCount < GYRO_BIAS_SAMPLES)
                            {
                                gyroZBiasSum += gyroZCal;
                                gyroBiasSampleCount++;
                            }
                            else
                            {
                                // Update adaptive bias
                                adaptiveGyroZBias = gyroZBiasSum / GYRO_BIAS_SAMPLES;
                                gyroZBiasSum = 0.0;
                                gyroBiasSampleCount = 0;
                            }
                        }
                    }
                    else
                    {
                        isStationary = false;
                        stationaryStartTime = millis();
                    }

                    // Apply adaptive bias correction to yaw gyro
                    float gyroZCorrected = gyroZCal - adaptiveGyroZBias;

                    // Complementary filter for stable angle estimation
                    // 98% gyro integration + 2% accelerometer correction
                    float dt = 0.01; // 10ms loop time
                    float alpha = 0.98;

                    imuData.roll = alpha * (imuData.roll + gyroXCal * dt) + (1.0 - alpha) * rollAccCal;
                    imuData.pitch = alpha * (imuData.pitch + gyroYCal * dt) + (1.0 - alpha) * pitchAccCal;

                    // Yaw with leaky integrator and bias correction
                    imuData.yaw = (imuData.yaw + gyroZCorrected * dt) * YAW_LEAKY_ALPHA;

                    // Store calibrated angular rates
                    imuData.rollRate = gyroXCal;
                    imuData.pitchRate = gyroYCal;
                    imuData.yawRate = gyroZCorrected; // Use bias-corrected yaw rate

                    imuData.dataValid = true;
                    imuData.timestamp = millis();
                }

                xSemaphoreGive(imuMutex);
            }
        }

        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, imuFrequency);
    }
}

// PID Control Task - Runs at 50Hz for smooth control
void pidControlTask(void *parameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t pidFrequency = pdMS_TO_TICKS(PID_LOOP_PERIOD); // 50Hz PID loop

    for (;;)
    {
        if (imuCalibration.calibrated && imuData.dataValid && pidEnabled)
        {
            // Take mutexes in consistent order to avoid deadlock
            if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(5)) == pdTRUE)
            {
                if (xSemaphoreTake(imuMutex, pdMS_TO_TICKS(5)) == pdTRUE)
                {
                    if (xSemaphoreTake(pidMutex, pdMS_TO_TICKS(5)) == pdTRUE)
                    {
                        // Determine flight mode and setpoints
                        updateFlightMode();

                        if (currentFlightMode >= FLIGHT_MODE_STABILIZE)
                        {
                            // Convert control inputs to angle setpoints with limits and smoothing
                            // Apply maximum angle limits for safety
                            float rollSetpointRaw = map(receivedControl.roll, -3000, 3000,
                                                        -pidConfig.max_angle, pidConfig.max_angle);
                            float pitchSetpointRaw = map(receivedControl.pitch, -3000, 3000,
                                                         -pidConfig.max_angle, pidConfig.max_angle);

                            // Constrain to safe limits
                            rollPID.setpoint = constrain(rollSetpointRaw, -pidConfig.max_angle, pidConfig.max_angle);
                            pitchPID.setpoint = constrain(pitchSetpointRaw, -pidConfig.max_angle, pidConfig.max_angle);

                            // Yaw is rate control (deg/s), not angle - apply rate limits
                            float yawRateSetpointRaw = map(receivedControl.yaw, -3000, 3000,
                                                           -pidConfig.max_yaw_rate, pidConfig.max_yaw_rate);
                            yawPID.setpoint = constrain(yawRateSetpointRaw, -pidConfig.max_yaw_rate, pidConfig.max_yaw_rate);

                            // Set current IMU readings as PID inputs
                            rollPID.input = imuData.roll;
                            pitchPID.input = imuData.pitch;
                            yawPID.input = imuData.yawRate; // Rate control for yaw

                            // Enable PID controllers
                            rollPID.enabled = true;
                            pitchPID.enabled = true;
                            yawPID.enabled = true;

                            // Compute PID outputs
                            pidRollOutput = rollPID.compute();
                            pidPitchOutput = pitchPID.compute();
                            pidYawOutput = yawPID.compute();

                            // Altitude hold mode with climb rate limiting
                            if (currentFlightMode >= FLIGHT_MODE_ALTITUDE_HOLD)
                            {
                                // Use pressure altitude for altitude hold
                                altitudePID.input = getFilteredAltitude();
                                altitudePID.enabled = true;

                                // Limit altitude changes to reasonable climb rate
                                float altitudeError = altitudePID.setpoint - altitudePID.input;
                                float maxAltitudeChange = pidConfig.max_climb_rate * (PID_LOOP_PERIOD / 1000.0);

                                if (abs(altitudeError) > maxAltitudeChange)
                                {
                                    // Gradually approach target altitude
                                    if (altitudeError > 0)
                                        altitudePID.setpoint = altitudePID.input + maxAltitudeChange;
                                    else
                                        altitudePID.setpoint = altitudePID.input - maxAltitudeChange;
                                }

                                pidAltitudeOutput = altitudePID.compute();
                            }
                            else
                            {
                                altitudePID.enabled = false;
                                pidAltitudeOutput = 0.0;
                            }
                        }
                        else
                        {
                            // Manual mode - disable all PID controllers
                            rollPID.enabled = false;
                            pitchPID.enabled = false;
                            yawPID.enabled = false;
                            altitudePID.enabled = false;

                            pidRollOutput = 0.0;
                            pidPitchOutput = 0.0;
                            pidYawOutput = 0.0;
                            pidAltitudeOutput = 0.0;
                        }

                        // Release mutexes in reverse order
                        xSemaphoreGive(pidMutex);
                    }
                    xSemaphoreGive(imuMutex);
                }
                xSemaphoreGive(controlMutex);
            }

            // Enhanced debugging output every 100 cycles (2 seconds at 50Hz)
            static int debugCounter = 0;
            if (++debugCounter >= 100)
            {
                debugCounter = 0;

                if (enableVerboseLogging && currentFlightMode >= FLIGHT_MODE_STABILIZE)
                {
                    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        // PID diagnostic information
                        Serial.print("PID Diagnostics - Mode: ");
                        Serial.print(currentFlightMode);
                        Serial.println();

                        // Roll axis diagnostics
                        Serial.print("  Roll: SP=");
                        Serial.print(rollPID.setpoint, 1);
                        Serial.print(" IN=");
                        Serial.print(rollPID.input, 1);
                        Serial.print(" OUT=");
                        Serial.print(pidRollOutput, 1);
                        Serial.print(" [P=");
                        Serial.print(rollPID.getProportionalTerm(), 1);
                        Serial.print(" I=");
                        Serial.print(rollPID.getIntegralTerm(), 1);
                        Serial.print(" D=");
                        Serial.print(rollPID.getDerivativeTerm(), 1);
                        Serial.print("]");
                        if (rollPID.isOutputSaturated())
                            Serial.print(" SAT");
                        if (rollPID.isIntegralSaturated())
                            Serial.print(" I-SAT");
                        Serial.println();

                        // Pitch axis diagnostics
                        Serial.print("  Pitch: SP=");
                        Serial.print(pitchPID.setpoint, 1);
                        Serial.print(" IN=");
                        Serial.print(pitchPID.input, 1);
                        Serial.print(" OUT=");
                        Serial.print(pidPitchOutput, 1);
                        Serial.print(" [P=");
                        Serial.print(pitchPID.getProportionalTerm(), 1);
                        Serial.print(" I=");
                        Serial.print(pitchPID.getIntegralTerm(), 1);
                        Serial.print(" D=");
                        Serial.print(pitchPID.getDerivativeTerm(), 1);
                        Serial.print("]");
                        if (pitchPID.isOutputSaturated())
                            Serial.print(" SAT");
                        if (pitchPID.isIntegralSaturated())
                            Serial.print(" I-SAT");
                        Serial.println();

                        // Yaw axis diagnostics (rate control)
                        Serial.print("  Yaw: SP=");
                        Serial.print(yawPID.setpoint, 1);
                        Serial.print(" IN=");
                        Serial.print(yawPID.input, 1);
                        Serial.print(" OUT=");
                        Serial.print(pidYawOutput, 1);
                        Serial.print(" [P=");
                        Serial.print(yawPID.getProportionalTerm(), 1);
                        Serial.print(" I=");
                        Serial.print(yawPID.getIntegralTerm(), 1);
                        Serial.print(" D=");
                        Serial.print(yawPID.getDerivativeTerm(), 1);
                        Serial.print("]");
                        if (yawPID.isOutputSaturated())
                            Serial.print(" SAT");
                        if (yawPID.isIntegralSaturated())
                            Serial.print(" I-SAT");
                        Serial.println();

                        // Altitude diagnostics if in altitude hold
                        if (currentFlightMode >= FLIGHT_MODE_ALTITUDE_HOLD)
                        {
                            Serial.print("  Alt: SP=");
                            Serial.print(altitudePID.setpoint, 2);
                            Serial.print(" IN=");
                            Serial.print(altitudePID.input, 2);
                            Serial.print(" OUT=");
                            Serial.print(pidAltitudeOutput, 1);
                            Serial.print(" [P=");
                            Serial.print(altitudePID.getProportionalTerm(), 1);
                            Serial.print(" I=");
                            Serial.print(altitudePID.getIntegralTerm(), 1);
                            Serial.print(" D=");
                            Serial.print(altitudePID.getDerivativeTerm(), 1);
                            Serial.print("]");
                            if (altitudePID.isOutputSaturated())
                                Serial.print(" SAT");
                            if (altitudePID.isIntegralSaturated())
                                Serial.print(" I-SAT");
                            Serial.println();
                        }

                        Serial.println("---");
                        xSemaphoreGive(serialMutex);
                    }
                }
            }
        }
        else
        {
            // PID disabled or IMU not ready
            if (xSemaphoreTake(pidMutex, pdMS_TO_TICKS(5)) == pdTRUE)
            {
                pidRollOutput = 0.0;
                pidPitchOutput = 0.0;
                pidYawOutput = 0.0;
                pidAltitudeOutput = 0.0;
                xSemaphoreGive(pidMutex);
            }
        }

        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, pidFrequency);
    }
}

// Update flight mode based on control inputs and system state
void updateFlightMode()
{
    static FlightMode previousMode = FLIGHT_MODE_DISARMED;

    if (!motorsArmed)
    {
        currentFlightMode = FLIGHT_MODE_DISARMED;
        pidEnabled = false;
    }
    else
    {
        // If motors are armed but still in disarmed mode, switch to manual
        if (currentFlightMode == FLIGHT_MODE_DISARMED)
        {
            currentFlightMode = FLIGHT_MODE_MANUAL;
            pidEnabled = false;
        }

        // Check joystick button states for mode switching
        // Joy1 button: Toggle between Manual and Stabilize
        // Joy2 button: Enable altitude hold (if in stabilize mode)

        static bool joy1ButtonPressed = false;
        static bool joy2ButtonPressed = false;

        // Joy1 button (toggle stabilization)
        if (receivedControl.joy1_btn == 0 && !joy1ButtonPressed) // Button pressed (active low)
        {
            joy1ButtonPressed = true;
            if (currentFlightMode == FLIGHT_MODE_MANUAL)
            {
                currentFlightMode = FLIGHT_MODE_STABILIZE;
                pidEnabled = true;

                // Reset PID controllers when enabling
                rollPID.reset();
                pitchPID.reset();
                yawPID.reset();
            }
            else if (currentFlightMode >= FLIGHT_MODE_STABILIZE)
            {
                currentFlightMode = FLIGHT_MODE_MANUAL;
                pidEnabled = false;
            }
        }
        else if (receivedControl.joy1_btn == 1) // Button released
        {
            joy1ButtonPressed = false;
        }

        // Joy2 button (altitude hold toggle)
        if (receivedControl.joy2_btn == 0 && !joy2ButtonPressed) // Button pressed
        {
            joy2ButtonPressed = true;
            if (currentFlightMode == FLIGHT_MODE_STABILIZE)
            {
                currentFlightMode = FLIGHT_MODE_ALTITUDE_HOLD;
                targetAltitude = getFilteredAltitude(); // Set current altitude as target
                altitudePID.setpoint = targetAltitude;
                altitudePID.reset();
            }
            else if (currentFlightMode == FLIGHT_MODE_ALTITUDE_HOLD)
            {
                currentFlightMode = FLIGHT_MODE_STABILIZE;
            }
        }
        else if (receivedControl.joy2_btn == 1) // Button released
        {
            joy2ButtonPressed = false;
        }

        // Default to manual mode on startup
        if (currentFlightMode == FLIGHT_MODE_DISARMED && motorsArmed)
        {
            currentFlightMode = FLIGHT_MODE_MANUAL;
            pidEnabled = false;
        }
    }

    // Print mode changes
    if (currentFlightMode != previousMode)
    {
        if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            Serial.print("Flight mode changed to: ");
            switch (currentFlightMode)
            {
            case FLIGHT_MODE_DISARMED:
                Serial.println("DISARMED");
                break;
            case FLIGHT_MODE_MANUAL:
                Serial.println("MANUAL");
                break;
            case FLIGHT_MODE_STABILIZE:
                Serial.println("STABILIZE");
                break;
            case FLIGHT_MODE_ALTITUDE_HOLD:
                Serial.println("ALTITUDE HOLD");
                break;
            case FLIGHT_MODE_POSITION_HOLD:
                Serial.println("POSITION HOLD");
                break;
            }
            xSemaphoreGive(serialMutex);
        }
        previousMode = currentFlightMode;
    }
}

// Get filtered altitude from barometric sensor
float getFilteredAltitude()
{
    // This is a simple implementation - you might want to enhance this
    // with more sophisticated filtering
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        float altitude = telemetryData.altitude / 100.0; // Convert cm to meters
        xSemaphoreGive(telemetryMutex);
        return altitude;
    }
    return 0.0;
}
