#include <Arduino.h>
#include <Bounce2.h>
#include <Keypad.h>

const int BUTTON_PIN = 0;
const int SWITCH_PIN = 4;

Bounce button = Bounce();
Bounce hookSwitch = Bounce();

const byte ROWS = 4;
const byte COLS = 3;

char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

byte rowPins[ROWS] = {14, 15, 17, 16};  // R1, R2, R3, R4
byte colPins[COLS] = {5, 6, 9};         // C1, C2, C3

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

void setup() {
  Serial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  button.attach(BUTTON_PIN);
  button.interval(10);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  hookSwitch.attach(SWITCH_PIN);
  hookSwitch.interval(10);
}

void loop() {
  button.update();
  if (button.changed()) {
    Serial.println(button.read() == LOW ? "Button: pressed" : "Button: released");
  }

  hookSwitch.update();
  if (hookSwitch.changed()) {
    Serial.println(hookSwitch.read() == LOW ? "Switch: closed" : "Switch: open");
  }

  char key = keypad.getKey();
  if (key) {
    Serial.print("Keypad: ");
    Serial.println(key);
  }
}
