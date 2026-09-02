#include <Arduino.h>
#include <Bounce2.h>
#include <Keypad.h>
#include <recordingstate.h>

const int BUTTON_PIN = 0;
const int SWITCH_PIN = 4;

bool MODE; //0 is playback mode, 1 is recording mode
RecordingState state = RecordingState::Idle;

Bounce phoneButton = Bounce();
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
  phoneButton.attach(BUTTON_PIN);
  phoneButton.interval(10);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  hookSwitch.attach(SWITCH_PIN);
  hookSwitch.interval(10);
  MODE = hookSwitch.read(); 
}

void loop() {
  //phone picked up or not
  phoneButton.update();
  if (phoneButton.changed() && phoneButton.read() == LOW) { //phone was picked up
    OnPhoneButtonPickedUp();
  }
  else if(phoneButton.changed() && phoneButton.read() == HIGH){ //phone was set Down
    OnPhoneButtonPutDown();
  }

  //check if record switch changed
  hookSwitch.update();
  if (hookSwitch.changed() && hookSwitch.read() == LOW) { //Playback Mode
    MODE = false;
  }
  else if(hookSwitch.changed() && hookSwitch.read() == HIGH) { //Recording Mode
    MODE = true;
  }

  if(MODE = false) { //playback mode logic
    char key = keypad.getKey();
    if (key) {
      Serial.print("Keypad: ");
      Serial.println(key);
    }
  }
}

void OnPhoneButtonPickedUp() {
  if(MODE == true) //recording mode
  {
    if(state == RecordingState::Idle) {
      //play prompt
      state = RecordingState::PlayingPrompt;

      //play beep
      state = RecordingState::PlayingBeep;

      //start recording
      state = RecordingState::Recording;
    }

  }
}

void OnPhoneButtonPutDown() {
  if(MODE == true) //recording mode
  {
    //stop prompt playback and beep
    
    if(state == RecordingState::Recording) {
      state = RecordingState::Saving;

      //stop recording and save

      //switch to idle
      state = RecordingState::Idle;
    }
  }
}

