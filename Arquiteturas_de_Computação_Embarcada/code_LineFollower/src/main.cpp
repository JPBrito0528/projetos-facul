#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <TimerOne.h>
#include <QTRSensors.h>

#define NUM_MOTORS 2
#define SensorCount 5
#define WHEEL_RADIUS 0.035
#define DT 0.040
#define WHEEL_DIST 0.125
#define NUM_SAMPLES 2

QTRSensors qtr;
volatile uint16_t position;
uint16_t sensorValues[SensorCount];
int sum_ir;

//PID CONTROL
float Kp = 10;
float Ki = 0  ;
float Kd = 2; 
volatile float P;
volatile float I;
volatile float D;

volatile float motorspeed;

volatile int lastError;
volatile int error;

boolean obstacle_dodge = false;

const int encoder0A_pin = 2;  // Motor1 encoder A input
const int encoder0B_pin = 3;  // Motor1 encoder B input
const int encoder1A_pin = 4;  // Motor2 encoder A input
const int encoder1B_pin = 5;  // Motor2 encoder B input

const int M1_dir_pin = 8;   // Motor1 Dir output
const int M2_dir_pin = 11;  // Motor2 Dir output

const int M1_PWM_pin = 9;   // Motor1 PWM output
const int M2_PWM_pin = 10;  // Motor2 PWM output

volatile int M_PWM_value[NUM_MOTORS];
uint8_t LED_state;

static void set_M1_PWM(volatile int new_PWM)
{
  if (new_PWM >= 0) {
    M_PWM_value[0] = new_PWM;
    digitalWrite(M1_dir_pin, false);
  }else if (new_PWM < 0){
    M_PWM_value[0] = -new_PWM;
    digitalWrite(M1_dir_pin, true);
  }
  Timer1.setPwmDuty(TIMER1_A_PIN, M_PWM_value[0]);
}


static void set_M2_PWM(volatile int new_PWM)
{
  if (new_PWM >= 0) {
      M_PWM_value[1] = new_PWM;
      digitalWrite(M2_dir_pin, false);
  }else if (new_PWM < 0){
      M_PWM_value[1] = -new_PWM;
      digitalWrite(M2_dir_pin, true);
  }
  Timer1.setPwmDuty(TIMER1_B_PIN, M_PWM_value[1]);
}

void calibration() {
  digitalWrite(LED_BUILTIN, HIGH);
  for (uint16_t i = 0; i < 400; i++)
  {
    qtr.calibrate();
  }
  digitalWrite(LED_BUILTIN, LOW);
}

void PID_control(int maxspeed, int minspeed, int basespeed) {
  error = 2000 - position;

  P = error;
  I += error;
  D = error - lastError;
  motorspeed = P*Kp + D*Kd + I*Ki;
  lastError = error;

  if (motorspeed > 200 || motorspeed < -200){
    I -= error;
  }

  M_PWM_value[0] = basespeed - motorspeed;
  M_PWM_value[1] = basespeed + motorspeed;
  
  if (M_PWM_value[0] > maxspeed) {
    M_PWM_value[0] = maxspeed;
  }else if (M_PWM_value[0] < minspeed){
    M_PWM_value[0] = minspeed;
  }

  if (M_PWM_value[1] > maxspeed) {
    M_PWM_value[1] = maxspeed;
  }else if (M_PWM_value[1] < minspeed){
    M_PWM_value[1] = minspeed;
  }
}


typedef struct {
  float ve, we;
  float ds, dtheta;
  float rel_s, rel_theta;
  float x, y, theta;
  
  uint8_t state, new_state, prev_state;
  uint32_t tis, tes;

  float dt;
  float v, w;
  float v_req, w_req;

} robot_t;

unsigned long interval;
unsigned long currentMicros, previousMicros;

volatile float odo[NUM_MOTORS];
float v_wheel[NUM_MOTORS];
float v_wheel_ref[NUM_MOTORS];

robot_t robot;

VL53L0X sensor;
float distance, prev_distance;

#pragma GCC push_options
#pragma GCC optimize ("O3")

static void set_motor_PWM(byte motor, volatile int new_PWM) // Motor control
{
  if (motor == 0) set_M1_PWM(new_PWM);
  else if (motor == 1) set_M2_PWM(new_PWM);
}

void set_robot_state(robot_t& robot, uint8_t new_state){
  if (robot.state != new_state) {  // if the state chnanged tis is reset
    robot.prev_state = robot.state;
    robot.state = new_state;
    robot.tes = millis();
    robot.tis = 0;
  }
}

void setup() {
  delay(100);                                 //stabilize voltage for VL53L0X sensor
  pinMode(encoder0A_pin, INPUT_PULLUP);
  pinMode(encoder0B_pin, INPUT_PULLUP);
  pinMode(encoder1A_pin, INPUT_PULLUP);
  pinMode(encoder1B_pin, INPUT_PULLUP);

  pinMode(M1_dir_pin, OUTPUT);
  pinMode(M2_dir_pin, OUTPUT);

  pinMode(M1_PWM_pin, OUTPUT);
  pinMode(M2_PWM_pin, OUTPUT);

  interval = 40UL * 1000UL;
  LED_state = 1;
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_state);

  Timer1.initialize(40); //uS
  Timer1.pwm(TIMER1_A_PIN, 0);
  Timer1.pwm(TIMER1_B_PIN, 0);
  set_motor_PWM(0, 0);
  set_motor_PWM(1, 0);

  Serial.begin(115200);
  Wire.begin();
  
  sensor.setTimeout(500);
  while (!sensor.init()) {
    Serial.println(F("Failed to detect and initialize sensor!"));
    delay(100);
  }

  qtr.setTypeAnalog();  //Analog Read IR Sensor
  qtr.setSensorPins((const uint8_t[]){1, 2, 3, 6, 7}, SensorCount);
  qtr.setSamplesPerSensor(NUM_SAMPLES); 
  calibration();
  
  previousMicros = micros();
  robot.state = 10; //Start with PID Control
}

void loop() {
  byte i, l;

  if(sensor.readRangeAvailable()) { // distance measurement
    prev_distance = distance;
    distance = sensor.readRangeMillimeters() * 1e-3;
  }

  currentMicros = micros();

  if (currentMicros - previousMicros >= interval) {
    previousMicros = currentMicros;

    LED_state = !LED_state;
    digitalWrite(LED_BUILTIN, LED_state);


    // Start new distance measure
    sensor.startReadRangeMillimeters();

    /*
    Conversion from PWM To Linear Velocity. Ideally we should use
    encoder but there is come kind of a malfunction
    */
    for(i = 0; i < NUM_MOTORS; i++) {  //Not working properly, too dependant on battery's voltage
      odo[i] = WHEEL_RADIUS*(M_PWM_value[i]*130L*0.10472)/1023L;  
    }

    robot.ve = (odo[0] + odo[1]) / 2;
    robot.we = (odo[0] - odo[1]) / WHEEL_DIST;

    robot.ds = robot.ve * DT;
    robot.dtheta = robot.we * DT;

    robot.rel_s += robot.ds;
    robot.rel_theta += robot.dtheta;

    v_wheel_ref[0] = robot.v + robot.w * WHEEL_DIST / 2;
    v_wheel_ref[1] = robot.v - robot.w * WHEEL_DIST / 2;

    position = qtr.readLineBlack(sensorValues);
      sum_ir = 0;
      for (i = 0; i < SensorCount; i++) {
        sum_ir += 1000 - sensorValues[i];
      }

    //Set the state
    robot.tis = millis() - robot.tes;
    if(prev_distance <= 0.2 && distance <= 0.2 && obstacle_dodge == false){
      set_robot_state(robot, 1);
    }else if(robot.state == 1 && robot.tis > 700){
      set_robot_state(robot, 2);
    }else if(robot.state == 2 && robot.tis > 1000){
      set_robot_state(robot, 3);
    }else if(robot.state == 3 && robot.tis > 2000){
      set_robot_state(robot, 4);
    }else if(robot.state == 4 && robot.tis > 1000){
      set_robot_state(robot, 5);
    }else if (robot.state == 5 && robot.tis > 3200){
      set_robot_state(robot, 6);
    }else if (robot.state == 6 && robot.tis > 1200){
      set_robot_state(robot, 7);
    }else if (robot.state == 7 && sum_ir > 1300){
      set_robot_state(robot, 8);
    }else if (robot.state == 8 && robot.tis > 1000){
      set_robot_state(robot, 10);
      obstacle_dodge = false;
    }
    

    //What to execute in each state
    if (robot.state == 1) {  // The robot is stopped
      PID_control(200, -200, 200);
      obstacle_dodge = true;    
    }else if (robot.state == 10) {  // Test
      PID_control(500, -200, 370);   
    }else if (robot.state == 2){
      M_PWM_value[0] = 200;
      M_PWM_value[1] = -200;  
    }else if (robot.state == 3){
      M_PWM_value[0] = 200;
      M_PWM_value[1] = 200; 
    }else if (robot.state == 4){
      M_PWM_value[0] = -200;
      M_PWM_value[1] = 200; 
    }else if (robot.state == 5){
      M_PWM_value[0] = 200;
      M_PWM_value[1] = 200; 
    }else if (robot.state == 6){
      M_PWM_value[0] = -200;
      M_PWM_value[1] = 200; 
    }else if (robot.state == 7){
      M_PWM_value[0] = 200;
      M_PWM_value[1] = 200;
    }else if (robot.state == 8){
      M_PWM_value[0] = 200;
      M_PWM_value[1] = -200;
    }

    //Set the speed of each motor
    for(l = 0; l < NUM_MOTORS; l++) {
      noInterrupts();
      set_motor_PWM(l,  M_PWM_value[l]);
      interrupts();
    }    

    // Show things:

    // The IR sensor values
    for (i = 0; i < SensorCount; i++) {
      Serial.print(sensorValues[i]);
      Serial.print(" ");
    }

    Serial.print(distance);
    
    // raw odometry
    Serial.print(F(" ["));
    Serial.print(odo[0]);
    Serial.print(F(", "));
    Serial.print(odo[1]);
    Serial.print(F("] "));

    // PWM values
    Serial.print(F(" {"));
    Serial.print(M_PWM_value[0]);
    Serial.print(F(", "));
    Serial.print(M_PWM_value[1]);
    Serial.print(F("} "));
    Serial.print(F(" ("));
    Serial.print(sum_ir);
    Serial.print(F(") "));

    // Estimated speeds
    Serial.print(F(" ["));
    Serial.print(robot.ve);
    Serial.print(F(", "));
    Serial.print(robot.we);
    Serial.print(F("] "));    

    // Reference speeds
    Serial.print(F(" ("));
    Serial.print(robot.v);
    Serial.print(F(", "));
    Serial.print(robot.w);
    Serial.print(F(") "));    

    Serial.print(F(" ("));
    Serial.print(robot.state);
    Serial.print(F(") "));

    Serial.print(F(" S:"));
    Serial.print(robot.rel_s);
    Serial.print(F(", Ang:"));
    Serial.print(robot.rel_theta);
    
    Serial.println();
  }
}