#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Stepper.h>

#define DHTTYPE DHT11

LiquidCrystal_I2C lcd(0x27, 16, 2);
int pinGnd  = 4;
int pinVcc  = 3;
int pinDht  = 2;
DHT dht(pinDht, DHTTYPE);

const int stepsPerRevolution = 200;
Stepper myStepper(stepsPerRevolution, 8, 9, 10, 11);

void setup() {
  Serial.begin(9600);

  pinMode(pinVcc, OUTPUT);
  pinMode(pinGnd, OUTPUT);
  digitalWrite(pinVcc, HIGH);
  digitalWrite(pinGnd, LOW);

  dht.begin();

  lcd.begin();
  lcd.backlight();

  myStepper.setSpeed(60);
}

void loop() {
  static unsigned long lastSend = 0;
  if (millis() - lastSend >= 10000) {
    lastSend = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) {
      Serial.println("ERR");  // 에러 깃발
    } else {
      // CSV 포맷: device_id,온도,습도
      Serial.print("ARDUINO1,");
      Serial.print(t, 2);
      Serial.print(",");
      Serial.println(h, 2);

      // LCD 출력
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("T:");
      lcd.print(t,1);
      lcd.print("C");
      lcd.setCursor(0,1);
      lcd.print("H:");
      lcd.print(h,1);
      lcd.print("%");
    }
  }


  // Pi로부터 명령 대기
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "MOTOR") {
      myStepper.step(stepsPerRevolution);
      Serial.println("MOTOR_DONE");
    }
  }
}