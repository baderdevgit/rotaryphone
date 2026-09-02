#include <Arduino.h>
#include <Bounce2.h>
#include <Keypad.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <recordingstate.h>

const int BUTTON_PIN = 0;
const int SWITCH_PIN = 4;

// TODO: verify this against your Rev D shield's silkscreen/datasheet -
// 10 is the SD chip-select pin on PJRC's official audio shield, but
// third-party revisions have been known to differ.
#define SDCARD_CS_PIN 10

// WAV format written for every recording
const uint32_t SAMPLE_RATE = 44100;
const uint16_t BITS_PER_SAMPLE = 16;
const uint16_t NUM_CHANNELS = 1;

// how many audio blocks to accumulate before each SD write
const int NBLOX = 4;

const char* COUNTER_FILE = "COUNTER.TXT";

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

// Playback path: SD card -> promptPlayer -> I2S output (the shield's codec)
AudioPlaySdWav promptPlayer;
AudioOutputI2S audioOutput;
AudioConnection patchCordL(promptPlayer, 0, audioOutput, 0);
AudioConnection patchCordR(promptPlayer, 1, audioOutput, 1);

// Recording path: I2S input (mic) -> recordQueue -> we drain it to SD ourselves
AudioInputI2S audioInput;
AudioRecordQueue recordQueue;
AudioConnection patchCordRecordIn(audioInput, 0, recordQueue, 0);

AudioControlSGTL5000 sgtl5000;

File frec;
uint32_t recByteSaved = 0;
uint32_t nextRecordingId = 0;
char recordingFilename[13]; // "RECxxxxx.WAV" (8.3 filename) + null terminator

void OnPhoneButtonPickedUp();
void OnPhoneButtonPutDown();
void PlayPrompt();
void WriteWavHeader(File &f, uint32_t dataBytes);
uint32_t LoadNextRecordingId();
void SaveNextRecordingId(uint32_t id);
void StartRecording();
void ContinueRecording();
void StopRecording();

void setup() {
  Serial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  phoneButton.attach(BUTTON_PIN);
  phoneButton.interval(10);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  hookSwitch.attach(SWITCH_PIN);
  hookSwitch.interval(10);
  MODE = hookSwitch.read();

  AudioMemory(60);
  sgtl5000.enable();
  sgtl5000.volume(0.5);

  if (!SD.begin(SDCARD_CS_PIN)) {
    Serial.println("SD card init failed - check SDCARD_CS_PIN");
  }

  nextRecordingId = LoadNextRecordingId();
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

  if(MODE == false) { //playback mode logic
    char key = keypad.getKey();
    if (key) {
      Serial.print("Keypad: ");
      Serial.println(key);
    }
  }

  // advance the recording-mode state machine once the current audio finishes
  if (state == RecordingState::PlayingPrompt && !promptPlayer.isPlaying()) {
    //prompt finished -> next is the beep (not implemented yet)
    state = RecordingState::PlayingBeep;
  }

  // recording must be drained every loop pass or the buffer overflows
  if (state == RecordingState::Recording) {
    ContinueRecording();
  }
}

void OnPhoneButtonPickedUp() {
  if(MODE == true) //recording mode
  {
    if(state == RecordingState::Idle) {
      PlayPrompt();
    }
  }
}

void OnPhoneButtonPutDown() {
  if(MODE == true) //recording mode
  {
    // hung up before the prompt finished on its own - cut it off immediately
    if (state == RecordingState::PlayingPrompt) {
      promptPlayer.stop();
    }

    if(state == RecordingState::Recording) {
      state = RecordingState::Saving;

      StopRecording();

      //switch to idle
      state = RecordingState::Idle;
    } else {
      state = RecordingState::Idle;
    }
  }
}

void PlayPrompt() {
  state = RecordingState::PlayingPrompt;
  // file must exist at the root of the SD card, 8.3 filename, PCM WAV
  promptPlayer.play("PROMPT.WAV");

  while (promptPlayer.isPlaying()) {
    // busy-wait until the file finishes
  }

  state = RecordingState::PlayingBeep;
  promptPlayer.play("BEEP.WAV");
  while (promptPlayer.isPlaying()){
  }

  state = RecordingState::Recording;
  StartRecording();
}

// Writes a canonical 44-byte PCM WAV header at the file's current position.
// Called twice per recording: once as a placeholder before any audio is
// written, and again (after seeking back to 0) once the real byte count
// is known, since the header's size fields can't be known until the
// recording ends.
void WriteWavHeader(File &f, uint32_t dataBytes) {
  uint32_t byteRate = SAMPLE_RATE * NUM_CHANNELS * BITS_PER_SAMPLE / 8;
  uint16_t blockAlign = NUM_CHANNELS * BITS_PER_SAMPLE / 8;
  uint32_t chunkSize = 36 + dataBytes;
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1; // PCM

  f.write("RIFF");
  f.write((byte*)&chunkSize, 4);
  f.write("WAVE");
  f.write("fmt ");
  f.write((byte*)&subchunk1Size, 4);
  f.write((byte*)&audioFormat, 2);
  f.write((byte*)&NUM_CHANNELS, 2);
  f.write((byte*)&SAMPLE_RATE, 4);
  f.write((byte*)&byteRate, 4);
  f.write((byte*)&blockAlign, 2);
  f.write((byte*)&BITS_PER_SAMPLE, 2);
  f.write("data");
  f.write((byte*)&dataBytes, 4);
}

// Reads the next recording ID from COUNTER_FILE on SD. Returns 0 if the
// file doesn't exist yet (first recording ever made on this card).
uint32_t LoadNextRecordingId() {
  File f = SD.open(COUNTER_FILE, FILE_READ);
  if (!f) {
    return 0;
  }
  uint32_t id = f.parseInt();
  f.close();
  return id;
}

void SaveNextRecordingId(uint32_t id) {
  SD.remove(COUNTER_FILE);
  File f = SD.open(COUNTER_FILE, FILE_WRITE);
  if (f) {
    f.println(id);
    f.close();
  }
}

void StartRecording() {
  snprintf(recordingFilename, sizeof(recordingFilename), "REC%05lu.WAV", nextRecordingId);

  if (SD.exists(recordingFilename)) {
    SD.remove(recordingFilename);
  }

  frec = SD.open(recordingFilename, FILE_WRITE);
  if (!frec) {
    Serial.println("Failed to open recording file");
    return;
  }

  recByteSaved = 0;
  WriteWavHeader(frec, 0); // placeholder - patched with the real size in StopRecording()

  recordQueue.begin();
}

void ContinueRecording() {
  if (recordQueue.available() >= NBLOX) {
    byte buffer[NBLOX * AUDIO_BLOCK_SAMPLES * sizeof(int16_t)];
    for (int i = 0; i < NBLOX; i++) {
      memcpy(buffer + i * AUDIO_BLOCK_SAMPLES * sizeof(int16_t),
             recordQueue.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
      recordQueue.freeBuffer();
    }
    frec.write(buffer, sizeof(buffer));
    recByteSaved += sizeof(buffer);
  }
}

void StopRecording() {
  recordQueue.end();
  while (recordQueue.available() > 0) {
    frec.write((byte*)recordQueue.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
    recordQueue.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES * sizeof(int16_t);
  }

  frec.seek(0);
  WriteWavHeader(frec, recByteSaved);
  frec.close();

  nextRecordingId++;
  SaveNextRecordingId(nextRecordingId);
}
